#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm.h"
#include "wasm-internal.h"
#include "wasm-parse.h"
#include "wasm-gen.h"

#define INITIAL_OUTPUT_BUFFER_CAPACITY (64 * 1024)
#define DEFAULT_MEMORY_EXPORT 1
#define DUMP_OCTETS_PER_LINE 16
#define DUMP_OCTETS_PER_GROUP 2

#define GLOBAL_HEADERS_OFFSET 8
#define GLOBAL_HEADER_SIZE 6
#define FUNC_HEADERS_OFFSET(num_globals) \
  (GLOBAL_HEADERS_OFFSET + (num_globals)*GLOBAL_HEADER_SIZE)
#define FUNC_HEADER_SIZE(num_args) (24 + (num_args))
#define SEGMENT_HEADER_SIZE 13

/* offsets from the start of a function header */
#define FUNC_SIG_SIZE(num_args) (2 + (num_args))
#define FUNC_HEADER_NAME_OFFSET(num_args) (FUNC_SIG_SIZE(num_args) + 0)
#define FUNC_HEADER_CODE_START_OFFSET(num_args) (FUNC_SIG_SIZE(num_args) + 4)
#define FUNC_HEADER_CODE_END_OFFSET(num_args) (FUNC_SIG_SIZE(num_args) + 8)
#define FUNC_HEADER_EXPORTED_OFFSET(num_args) (FUNC_SIG_SIZE(num_args) + 20)

/* offsets from the start of a segment header */
#define SEGMENT_HEADER_DATA_OFFSET 4

typedef struct OutputBuffer {
  void* start;
  size_t size;
  size_t capacity;
} OutputBuffer;

typedef struct Context {
  WasmModule* module;
  OutputBuffer* buf;
  uint32_t function_num_exprs_offset;
  /* offset of each function/segment header in buf. */
  uint32_t* function_header_offsets;
  uint32_t* segment_header_offsets;
} Context;

extern const char* g_outfile;
extern int g_dump_module;
extern int g_verbose;

static const char* s_opcode_names[] = {
#define OPCODE(name, code) [code] = "OPCODE_" #name,
    OPCODES(OPCODE)
#undef OPCODE
};

static uint32_t log_two_u32(uint32_t x) {
  if (!x)
    return 0;
  return sizeof(unsigned int) * 8 - __builtin_clz(x - 1);
}

static void init_output_buffer(OutputBuffer* buf, size_t initial_capacity) {
  /* We may be reusing the buffer, free it */
  free(buf->start);
  buf->start = malloc(initial_capacity);
  if (!buf->start)
    FATAL("unable to allocate %zd bytes\n", initial_capacity);
  buf->size = 0;
  buf->capacity = initial_capacity;
}

static void ensure_output_buffer_capacity(OutputBuffer* buf,
                                          size_t ensure_capacity) {
  if (ensure_capacity > buf->capacity) {
    size_t new_capacity = buf->capacity * 2;
    while (new_capacity < ensure_capacity)
      new_capacity *= 2;
    buf->start = realloc(buf->start, new_capacity);
    if (!buf->start)
      FATAL("unable to allocate %zd bytes\n", new_capacity);
    buf->capacity = new_capacity;
  }
}

static void dump_memory(const void* start,
                        size_t size,
                        size_t offset,
                        int print_chars,
                        const char* desc) {
  /* mimic xxd output */
  const uint8_t* p = start;
  const uint8_t* end = p + size;
  while (p < end) {
    const uint8_t* line = p;
    const uint8_t* line_end = p + DUMP_OCTETS_PER_LINE;
    printf("%07x: ", (int)((void*)p - start + offset));
    while (p < line_end) {
      int i;
      for (i = 0; i < DUMP_OCTETS_PER_GROUP; ++i, ++p) {
        if (p < end) {
          printf("%02x", *p);
        } else {
          putchar(' ');
          putchar(' ');
        }
      }
      putchar(' ');
    }

    putchar(' ');
    p = line;
    int i;
    for (i = 0; i < DUMP_OCTETS_PER_LINE && p < end; ++i, ++p)
      if (print_chars)
        printf("%c", isprint(*p) ? *p : '.');
    /* if there are multiple lines, only print the desc on the last one */
    if (p >= end && desc)
      printf("  ; %s", desc);
    putchar('\n');
  }
}

static size_t out_data(OutputBuffer* buf,
                       size_t offset,
                       const void* src,
                       size_t size,
                       const char* desc) {
  assert(offset <= buf->size);
  ensure_output_buffer_capacity(buf, offset + size);
  memcpy(buf->start + offset, src, size);
  if (g_verbose)
    dump_memory(buf->start + offset, size, offset, 0, desc);
  return offset + size;
}

/* TODO(binji): endianness */
#define OUT_TYPE(name, ctype)                                          \
  void out_##name(OutputBuffer* buf, ctype value, const char* desc) {  \
    buf->size = out_data(buf, buf->size, &value, sizeof(value), desc); \
  }

OUT_TYPE(u8, uint8_t)
OUT_TYPE(u16, uint16_t)
OUT_TYPE(u32, uint32_t)
OUT_TYPE(u64, uint64_t)
OUT_TYPE(f32, float)
OUT_TYPE(f64, double)

#undef OUT_TYPE

#define OUT_AT_TYPE(name, ctype)                                        \
  void out_##name##_at(OutputBuffer* buf, uint32_t offset, ctype value, \
                       const char* desc) {                              \
    out_data(buf, offset, &value, sizeof(value), desc);                 \
  }

OUT_AT_TYPE(u8, uint8_t)
OUT_AT_TYPE(u32, uint32_t)

#undef OUT_AT_TYPE

void out_opcode(OutputBuffer* buf, WasmOpcode opcode) {
  out_u8(buf, (uint8_t)opcode, s_opcode_names[opcode]);
}

void out_leb128(OutputBuffer* buf, uint32_t value, const char* desc) {
  uint8_t data[5]; /* max 5 bytes */
  int i = 0;
  do {
    assert(i < 5);
    data[i] = value & 0x7f;
    value >>= 7;
    if (value)
      data[i] |= 0x80;
    ++i;
  } while (value);
  buf->size = out_data(buf, buf->size, data, i, desc);
}

static void out_cstr(OutputBuffer* buf, const char* s, const char* desc) {
  buf->size = out_data(buf, buf->size, s, strlen(s) + 1, desc);
}

static void out_segment(OutputBuffer* buf,
                        WasmSegment* segment,
                        const char* desc) {
  size_t offset = buf->size;
  ensure_output_buffer_capacity(buf, offset + segment->size);
  void* dest = buf->start + offset;
  wasm_copy_segment_data(segment->data, dest, segment->size);
  if (g_verbose)
    dump_memory(buf->start + offset, segment->size, offset, 1, desc);
  buf->size += segment->size;
}

static void dump_output_buffer(OutputBuffer* buf) {
  dump_memory(buf->start, buf->size, 0, 1, NULL);
}

static void write_output_buffer(OutputBuffer* buf, const char* filename) {
  FILE* out = (strcmp(filename, "-") != 0) ? fopen(filename, "wb") : stdout;
  if (!out)
    FATAL("unable to open %s\n", filename);
  if (fwrite(buf->start, 1, buf->size, out) != buf->size)
    FATAL("unable to write %zd bytes\n", buf->size);
  fclose(out);
}

static void destroy_output_buffer(OutputBuffer* buf) {
  free(buf->start);
}

static void out_module_header(Context* ctx, WasmModule* module) {
  OutputBuffer* buf = ctx->buf;
  out_u8(buf, log_two_u32(module->max_memory_size), "mem size log 2");
  out_u8(buf, DEFAULT_MEMORY_EXPORT, "export mem");
  out_u16(buf, module->globals.size, "num globals");
  out_u16(buf, module->imports.size + module->functions.size, "num funcs");
  out_u16(buf, module->segments.size, "num data segments");

  int i;
  for (i = 0; i < module->globals.size; ++i) {
    WasmVariable* global = &module->globals.data[i];
    if (g_verbose)
      printf("; global header %d\n", i);
    const uint8_t global_type_codes[WASM_NUM_TYPES] = {-1, 4, 6, 8, 9};
    out_u32(buf, 0, "global name offset");
    out_u8(buf, global_type_codes[global->type], "global mem type");
    out_u8(buf, 0, "export global");
  }

  for (i = 0; i < module->imports.size; ++i) {
    WasmImport* import = &module->imports.data[i];
    if (g_verbose)
      printf("; import header %d\n", i);

    out_u8(buf, import->args.size, "import num args");
    out_u8(buf, import->result_type, "import result_type");

    int j;
    for (j = 0; j < import->args.size; ++j)
      out_u8(buf, import->args.data[j].type, "import arg type");

    out_u32(buf, 0, "import name offset");
    out_u32(buf, 0, "import code start offset");
    out_u32(buf, 0, "import code end offset");
    out_u16(buf, 0, "num local i32");
    out_u16(buf, 0, "num local i64");
    out_u16(buf, 0, "num local f32");
    out_u16(buf, 0, "num local f64");
    out_u8(buf, 0, "export func");
    out_u8(buf, 1, "import external");
  }

  for (i = 0; i < module->functions.size; ++i) {
    WasmFunction* function = &module->functions.data[i];
    if (g_verbose)
      printf("; function header %d\n", i);

    out_u8(buf, function->num_args, "func num args");
    out_u8(buf, function->result_type, "func result type");
    int j;
    for (j = 0; j < function->num_args; ++j)
      out_u8(buf, function->locals.data[j].type, "func arg type");
    out_u32(buf, 0, "func name offset");
    out_u32(buf, 0, "func code start offset");
    out_u32(buf, 0, "func code end offset");

    int num_locals[WASM_NUM_TYPES] = {};
    for (j = function->num_args; j < function->locals.size; ++j)
      num_locals[function->locals.data[j].type]++;

    out_u16(buf, num_locals[WASM_TYPE_I32], "num local i32");
    out_u16(buf, num_locals[WASM_TYPE_I64], "num local i64");
    out_u16(buf, num_locals[WASM_TYPE_F32], "num local f32");
    out_u16(buf, num_locals[WASM_TYPE_F64], "num local f64");
    out_u8(buf, 0, "export func");
    out_u8(buf, 0, "func external");
  }

  for (i = 0; i < module->segments.size; ++i) {
    WasmSegment* segment = &module->segments.data[i];
    if (g_verbose)
      printf("; segment header %d\n", i);
    out_u32(buf, segment->address, "segment address");
    out_u32(buf, 0, "segment data offset");
    out_u32(buf, segment->size, "segment size");
    out_u8(buf, 1, "segment init");
  }
}

static void out_module_footer(Context* ctx, WasmModule* module) {
  OutputBuffer* buf = ctx->buf;
  int i;
  for (i = 0; i < module->segments.size; ++i) {
    if (g_verbose)
      printf("; segment data %d\n", i);
    WasmSegment* segment = &module->segments.data[i];
    out_u32_at(buf, ctx->segment_header_offsets[i] + SEGMENT_HEADER_DATA_OFFSET,
               buf->size, "FIXUP segment data offset");
    out_segment(buf, segment, "segment data");
  }

  /* output name table */
  if (g_verbose)
    printf("; names\n");
  uint32_t offset = FUNC_HEADERS_OFFSET(module->globals.size);
  for (i = 0; i < module->imports.size; ++i) {
    WasmImport* import = &module->imports.data[i];
    out_u32_at(buf, offset + FUNC_HEADER_NAME_OFFSET(import->args.size),
               buf->size, "FIXUP import name offset");
    out_cstr(buf, import->func_name, "import name");
    offset += FUNC_HEADER_SIZE(import->args.size);
  }
  for (i = 0; i < module->functions.size; ++i) {
    WasmFunction* function = &module->functions.data[i];
    if (function->exported) {
      out_u32_at(buf, offset + FUNC_HEADER_NAME_OFFSET(function->num_args),
                 buf->size, "FIXUP func name offset");
      out_cstr(buf, function->exported_name, "export name");
    }
    offset += FUNC_HEADER_SIZE(function->num_args);
  }
}

static void error(WasmSourceLocation loc, const char* msg, void* user_data) {
  fprintf(stderr, "%s:%d:%d: %s", loc.source->filename, loc.line, loc.col, msg);
}

static void before_module(WasmModule* module, void* user_data) {
  Context* ctx = user_data;
  ctx->function_header_offsets = realloc(
      ctx->function_header_offsets, module->functions.size * sizeof(uint32_t));
  int i;
  uint32_t offset = FUNC_HEADERS_OFFSET(module->globals.size);
  /* skip past the import headers */
  for (i = 0; i < module->imports.size; ++i)
    offset += FUNC_HEADER_SIZE(module->imports.data[i].args.size);

  for (i = 0; i < module->functions.size; ++i) {
    WasmFunction* function = &module->functions.data[i];
    ctx->function_header_offsets[i] = offset;
    offset += FUNC_HEADER_SIZE(function->num_args);
  }

  ctx->segment_header_offsets = realloc(
      ctx->segment_header_offsets, module->segments.size * sizeof(uint32_t));
  for (i = 0; i < module->segments.size; ++i) {
    ctx->segment_header_offsets[i] = offset;
    offset += SEGMENT_HEADER_SIZE;
  }

  ctx->module = module;
  init_output_buffer(ctx->buf, INITIAL_OUTPUT_BUFFER_CAPACITY);
  out_module_header(ctx, module);
}

static void after_module(WasmModule* module, void* user_data) {
  Context* ctx = user_data;
  out_module_footer(ctx, module);
  if (g_dump_module)
    dump_output_buffer(ctx->buf);
}

static void before_function(WasmModule* module,
                            WasmFunction* function,
                            void* user_data) {
  Context* ctx = user_data;
  int function_index = function - module->functions.data;
  if (g_verbose)
    printf("; function data %d\n", function_index);
  out_u32_at(ctx->buf, ctx->function_header_offsets[function_index] +
                           FUNC_HEADER_CODE_START_OFFSET(function->num_args),
             ctx->buf->size, "FIXUP func code start offset");
  /* The v8-native-prototype requires all functions to have a toplevel
   block */
  out_opcode(ctx->buf, WASM_OPCODE_BLOCK);
  ctx->function_num_exprs_offset = ctx->buf->size;
  out_u8(ctx->buf, 0, "toplevel block num expressions");
}

static void after_function(WasmModule* module,
                           WasmFunction* function,
                           int num_exprs,
                           void* user_data) {
  Context* ctx = user_data;
  int function_index = function - module->functions.data;
  out_u8_at(ctx->buf, ctx->function_num_exprs_offset, num_exprs,
            "FIXUP toplevel block num expressions");
  out_u32_at(ctx->buf, ctx->function_header_offsets[function_index] +
                           FUNC_HEADER_CODE_END_OFFSET(function->num_args),
             ctx->buf->size, "FIXUP func code end offset");
}

static void before_export(WasmModule* module, void* user_data) {}

static void after_export(WasmModule* module,
                         WasmFunction* function,
                         void* user_data) {
  Context* ctx = user_data;
  int function_index = function - module->functions.data;
  out_u8_at(ctx->buf, ctx->function_header_offsets[function_index] +
                          FUNC_HEADER_EXPORTED_OFFSET(function->num_args),
            1, "FIXUP func exported");
}

static WasmParserCookie before_block(void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_BLOCK);
  WasmParserCookie cookie = (WasmParserCookie)ctx->buf->size;
  out_u8(ctx->buf, 0, "num expressions");
  return cookie;
}

static void after_block(int num_exprs,
                        WasmParserCookie cookie,
                        void* user_data) {
  Context* ctx = user_data;
  uint32_t offset = (uint32_t)cookie;
  out_u8_at(ctx->buf, offset, num_exprs, "FIXUP num expressions");
}

static void before_binary(enum WasmOpcode opcode, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, opcode);
}

static void after_break(int depth, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_BREAK);
  out_u8(ctx->buf, depth, "break depth");
}

static void before_call(int function_index, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_CALL);
  /* defined functions are always after all imports */
  out_leb128(ctx->buf, ctx->module->imports.size + function_index,
             "func index");
}

static void before_call_import(int import_index, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_CALL);
  out_leb128(ctx->buf, import_index, "import index");
}

static void before_compare(enum WasmOpcode opcode, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, opcode);
}

static void after_const(enum WasmOpcode opcode,
                        WasmType type,
                        WasmNumber value,
                        void* user_data) {
  Context* ctx = user_data;
  switch (type) {
    case WASM_TYPE_I32: {
      int32_t i32 = value.i32;
      if (i32 >= -128 && i32 < 127) {
        out_opcode(ctx->buf, WASM_OPCODE_I8_CONST);
        out_u8(ctx->buf, value.i32, "u8 literal");
      } else {
        out_opcode(ctx->buf, opcode);
        out_u32(ctx->buf, value.i32, "u32 literal");
      }
      break;
    }

    case WASM_TYPE_I64:
      out_opcode(ctx->buf, opcode);
      out_u64(ctx->buf, value.i64, "u64 literal");
      break;

    case WASM_TYPE_F32:
      out_opcode(ctx->buf, opcode);
      out_f32(ctx->buf, value.f32, "f32 literal");
      break;

    case WASM_TYPE_F64:
      out_opcode(ctx->buf, opcode);
      out_f64(ctx->buf, value.f64, "f64 literal");
      break;

    default:
      assert(0);
      break;
  }
}

static void before_convert(enum WasmOpcode opcode, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, opcode);
}

static void after_get_local(int remapped_index, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_GET_LOCAL);
  out_leb128(ctx->buf, remapped_index, "remapped local index");
}

static WasmParserCookie before_if(void* user_data) {
  Context* ctx = user_data;
  uint32_t offset = ctx->buf->size;
  out_opcode(ctx->buf, WASM_OPCODE_IF);
  return (WasmParserCookie)offset;
}

static void after_if(int with_else, WasmParserCookie cookie, void* user_data) {
  Context* ctx = user_data;
  if (with_else) {
    uint32_t offset = (uint32_t)cookie;
    out_u8_at(ctx->buf, offset, WASM_OPCODE_IF_THEN, "FIXUP OPCODE_IF_THEN");
  }
}

static WasmParserCookie before_label(void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_BLOCK);
  WasmParserCookie cookie = (WasmParserCookie)ctx->buf->size;
  out_u8(ctx->buf, 0, "num expressions");
  return cookie;
}

static void after_label(int num_exprs,
                        WasmParserCookie cookie,
                        void* user_data) {
  Context* ctx = user_data;
  uint32_t offset = (uint32_t)cookie;
  out_u8_at(ctx->buf, offset, num_exprs, "FIXUP num expressions");
}

static void before_load(enum WasmOpcode opcode,
                        uint8_t access,
                        void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, opcode);
  out_u8(ctx->buf, access, "load access byte");
}

static void after_load_global(int index, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_GET_GLOBAL);
  out_leb128(ctx->buf, index, "global index");
}

static WasmParserCookie before_loop(void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_LOOP);
  WasmParserCookie cookie = (WasmParserCookie)ctx->buf->size;
  out_u8(ctx->buf, 0, "num expressions");
  return cookie;
}

static void after_loop(int num_exprs,
                       WasmParserCookie cookie,
                       void* user_data) {
  Context* ctx = user_data;
  uint32_t offset = (uint32_t)cookie;
  out_u8_at(ctx->buf, offset, num_exprs, "FIXUP num expressions");
}

static void after_nop(void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_NOP);
}

static void before_return(void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_RETURN);
}

static void before_set_local(int index, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_SET_LOCAL);
  out_leb128(ctx->buf, index, "remapped local index");
}

static void before_store(enum WasmOpcode opcode,
                         uint8_t access,
                         void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, opcode);
  out_u8(ctx->buf, access, "store access byte");
}

static void before_store_global(int index, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, WASM_OPCODE_SET_GLOBAL);
  out_leb128(ctx->buf, index, "global index");
}

static void before_unary(enum WasmOpcode opcode, void* user_data) {
  Context* ctx = user_data;
  out_opcode(ctx->buf, opcode);
}

static void assert_invalid_error(WasmSourceLocation loc,
                                 const char* msg,
                                 void* user_data) {
  fprintf(stdout, "assert_invalid error:\n  %s:%d:%d: %s", loc.source->filename,
          loc.line, loc.col, msg);
}

int wasm_gen_file(WasmSource* source, int multi_module) {
  OutputBuffer buf = {};

  Context ctx = {};
  ctx.buf = &buf;

  WasmParserCallbacks callbacks = {};
  callbacks.user_data = &ctx;
  callbacks.error = error;
  callbacks.before_module = before_module;
  callbacks.after_module = after_module;
  callbacks.before_function = before_function;
  callbacks.after_function = after_function;
  callbacks.before_export = before_export;
  callbacks.after_export = after_export;
  callbacks.before_block = before_block;
  callbacks.after_block = after_block;
  callbacks.before_binary = before_binary;
  callbacks.after_break = after_break;
  callbacks.before_call = before_call;
  callbacks.before_call_import = before_call_import;
  callbacks.before_compare = before_compare;
  callbacks.after_const = after_const;
  callbacks.before_convert = before_convert;
  callbacks.after_get_local = after_get_local;
  callbacks.before_if = before_if;
  callbacks.after_if = after_if;
  callbacks.before_label = before_label;
  callbacks.after_label = after_label;
  callbacks.before_load = before_load;
  callbacks.after_load_global = after_load_global;
  callbacks.before_loop = before_loop;
  callbacks.after_loop = after_loop;
  callbacks.after_nop = after_nop;
  callbacks.before_return = before_return;
  callbacks.before_set_local = before_set_local;
  callbacks.before_store = before_store;
  callbacks.before_store_global = before_store_global;
  callbacks.before_unary = before_unary;
  callbacks.assert_invalid_error = assert_invalid_error;

  int result;
  if (multi_module) {
    result = wasm_parse_file(source, &callbacks);
  } else {
    result = wasm_parse_module(source, &callbacks);
    if (result == 0 && g_outfile)
      write_output_buffer(&buf, g_outfile);
  }

  destroy_output_buffer(&buf);
  free(ctx.function_header_offsets);
  free(ctx.segment_header_offsets);
  return result;
}