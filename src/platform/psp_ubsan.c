#if defined(PORTABLE) && defined(__PSP__) && defined(PSP_UBSAN)
// For PSP_TEST builds with -fsanitize=alignment (PSP_UBSAN=1): reports each
// misaligned access once, through printf, which PPSSPPHeadless -l shows. The
// PSP faults on them, but PPSSPP doesn't.
#include <stdio.h>
#include <stdint.h>

struct SourceLocation
{
    const char *file;
    uint32_t line;
    uint32_t column;
};

struct TypeMismatchData
{
    struct SourceLocation location;
    const void *type;
    unsigned char logAlignment;
    unsigned char typeCheckKind;
};

#define MAX_REPORTS 256
static struct SourceLocation sReported[MAX_REPORTS];
static int sNumReported;

__attribute__((no_sanitize("alignment")))
static void Report(struct TypeMismatchData *data, uintptr_t pointer)
{
    uintptr_t alignment = (uintptr_t)1 << data->logAlignment;

    if (pointer == 0 || (pointer & (alignment - 1)) == 0)
        return;
    for (int i = 0; i < sNumReported; i++)
    {
        if (sReported[i].file == data->location.file && sReported[i].line == data->location.line)
            return;
    }
    if (sNumReported < MAX_REPORTS)
        sReported[sNumReported++] = data->location;
    printf("MISALIGNED %s:%u:%u address %08x needs %u\n", data->location.file, (unsigned)data->location.line,
           (unsigned)data->location.column, (unsigned)pointer, (unsigned)alignment);
}

void __ubsan_handle_type_mismatch_v1(struct TypeMismatchData *data, uintptr_t pointer)
{
    Report(data, pointer);
}

void __ubsan_handle_type_mismatch_v1_abort(struct TypeMismatchData *data, uintptr_t pointer)
{
    Report(data, pointer);
}
#endif
