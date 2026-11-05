#!/bin/bash
# Builds and runs the D3D9 shader bytecode and vertex emulation unit tests (the Catch tests in
# renderdoc/driver/d3d9/d3d9_bytecode.cpp, d3d9_shader_reflect.cpp, d3d9_vs_emulate.cpp and
# d3d9_ffp_vertex.cpp) on Linux, without building
# the rest of RenderDoc. On Windows they're part of the normal build: run
# `renderdoccmd test unit "[d3d9]"` from a Development build.
#
# usage: util/test/d3d9_shader_unit_tests.sh [build dir] [Catch arguments]
#
# Needs g++ with AddressSanitizer and UBSan. The handful of RenderDoc functions the tests need that
# live in files depending on most of RenderDoc are implemented minimally here, or (for
# StandardFillCBufferVariables) taken from replay_driver.cpp as they are.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$(mktemp -d)}
shift || true
mkdir -p "$OUT"

FLAGS="-std=c++17 -g -O1 -DRENDERDOC_PLATFORM_LINUX -I$ROOT/renderdoc -I$ROOT/renderdoc/3rdparty \
  -fsanitize=address,undefined -Wall -Wno-unknown-pragmas -Wno-class-memaccess -Wno-reorder"

cat > "$OUT/main.cpp" <<'EOF'
#define CATCH_CONFIG_RUNNER
#include <stdarg.h>
#include <stdlib.h>
#include "api/replay/renderdoc_replay.h"
#include "catch/catch.hpp"
#include "common/common.h"
#include "common/formatting.h"
#include "os/os_specific.h"

#include "api/replay/renderdoc_tostr.inl"

int utf8printv(char *buf, size_t bufSize, const char *fmt, va_list args);

static rdcstr VFmt(const char *format, va_list args)
{
  va_list args2;
  va_copy(args2, args);
  int size = ::utf8printv(NULL, 0, format, args2);
  va_end(args2);
  rdcstr ret;
  ret.resize(size);
  ::utf8printv(ret.data(), size + 1, format, args);
  return ret;
}

namespace StringFormat
{
rdcstr Fmt(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  rdcstr ret = VFmt(format, args);
  va_end(args);
  return ret;
}

rdcstr Fmt(rdcliteral format, ...)
{
  va_list args;
  va_start(args, format);
  rdcstr ret = VFmt(format.c_str(), args);
  va_end(args);
  return ret;
}

rdcstr Wide2UTF8(const rdcwstr &s)
{
  return rdcstr();
}
};

template <>
rdcstr DoStringise(const Catch::SourceLineInfo &el)
{
  return StringFormat::Fmt("%s:%zu", el.file, el.line);
}
#define BASIC_TOSTR(type, fmt, cast)       \
  template <>                              \
  rdcstr DoStringise(const type &el)       \
  {                                        \
    return StringFormat::Fmt(fmt, cast el); \
  }
BASIC_TOSTR(uint8_t, "%u", (uint32_t))
BASIC_TOSTR(uint16_t, "%u", (uint32_t))
BASIC_TOSTR(uint32_t, "%u", (uint32_t))
BASIC_TOSTR(int32_t, "%d", (int32_t))
BASIC_TOSTR(uint64_t, "%llu", (unsigned long long))
BASIC_TOSTR(float, "%f", (double))

template <>
rdcstr DoStringise(const bool &el)
{
  return el ? "true" : "false";
}

extern "C" void RENDERDOC_CC RENDERDOC_FreeArrayMem(void *mem)
{
  free(mem);
}

extern "C" void *RENDERDOC_CC RENDERDOC_AllocArrayMem(uint64_t sz)
{
  return malloc((size_t)sz);
}

int main(int argc, char **argv)
{
  return Catch::Session().run(argc, argv);
}
EOF

{
  echo '#include <string.h>'
  echo '#include "api/replay/renderdoc_replay.h"'
  echo '#include "common/common.h"'
  echo '#include "common/formatting.h"'
  sed -n '/^void StandardFillCBufferVariable(ResourceId shader/,/^void PreprocessLineDirectives/p' \
    "$ROOT/renderdoc/replay/replay_driver.cpp" | sed '$d'
  sed -n '/^void StandardFillCBufferVariables(ResourceId shader, const rdcarray<ShaderConstant> &invars,$/,/^}/p' \
    "$ROOT/renderdoc/replay/replay_driver.cpp"
} > "$OUT/fill.cpp"

OBJS=""
for s in "$ROOT/renderdoc/driver/d3d9/d3d9_bytecode.cpp" "$ROOT/renderdoc/driver/d3d9/d3d9_shader_reflect.cpp" \
  "$ROOT/renderdoc/driver/d3d9/d3d9_vs_emulate.cpp" "$ROOT/renderdoc/driver/d3d9/d3d9_ffp_vertex.cpp"; do
  o="$OUT/$(basename "$s" .cpp).o"
  g++ $FLAGS -c "$s" -o "$o"
  OBJS="$OBJS $o"
done
for s in strings/utf8printf.cpp strings/grisu2.cpp strings/string_utils.cpp; do
  o="$OUT/$(basename "$s" .cpp).o"
  g++ $FLAGS -w -include api/replay/renderdoc_replay.h -c "$ROOT/renderdoc/$s" -o "$o"
  OBJS="$OBJS $o"
done
g++ $FLAGS -w -c "$OUT/main.cpp" -o "$OUT/main.o"
g++ $FLAGS -w -c "$OUT/fill.cpp" -o "$OUT/fill.o"
g++ $FLAGS $OBJS "$OUT/main.o" "$OUT/fill.o" -o "$OUT/d3d9_shader_tests"

# rdcarray copies empty arrays with memcpy(NULL, NULL, 0), which UBSan reports. It's harmless.
"$OUT/d3d9_shader_tests" "${@:-[d3d9]}"
