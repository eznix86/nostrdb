#ifndef PROFILE_BUILDER_H
#define PROFILE_BUILDER_H

/* Generated by flatcc 0.6.1 FlatBuffers schema compiler for C by dvide.com */

#ifndef PROFILE_READER_H
#include "profile_reader.h"
#endif
#ifndef FLATBUFFERS_COMMON_BUILDER_H
#include "flatbuffers_common_builder.h"
#endif
#include "flatcc/flatcc_prologue.h"
#ifndef flatbuffers_identifier
#define flatbuffers_identifier 0
#endif
#ifndef flatbuffers_extension
#define flatbuffers_extension "bin"
#endif

static const flatbuffers_voffset_t __NdbProfile_required[] = { 0 };
typedef flatbuffers_ref_t NdbProfile_ref_t;
static NdbProfile_ref_t NdbProfile_clone(flatbuffers_builder_t *B, NdbProfile_table_t t);
__flatbuffers_build_table(flatbuffers_, NdbProfile, 12)

#define __NdbProfile_formal_args ,\
  flatbuffers_string_ref_t v0, flatbuffers_string_ref_t v1, flatbuffers_string_ref_t v2, flatbuffers_string_ref_t v3,\
  flatbuffers_string_ref_t v4, flatbuffers_string_ref_t v5, flatbuffers_bool_t v6, flatbuffers_string_ref_t v7,\
  flatbuffers_string_ref_t v8, int32_t v9, int32_t v10, flatbuffers_string_ref_t v11
#define __NdbProfile_call_args ,\
  v0, v1, v2, v3,\
  v4, v5, v6, v7,\
  v8, v9, v10, v11
static inline NdbProfile_ref_t NdbProfile_create(flatbuffers_builder_t *B __NdbProfile_formal_args);
__flatbuffers_build_table_prolog(flatbuffers_, NdbProfile, NdbProfile_file_identifier, NdbProfile_type_identifier)

__flatbuffers_build_string_field(0, flatbuffers_, NdbProfile_name, NdbProfile)
__flatbuffers_build_string_field(1, flatbuffers_, NdbProfile_website, NdbProfile)
__flatbuffers_build_string_field(2, flatbuffers_, NdbProfile_about, NdbProfile)
__flatbuffers_build_string_field(3, flatbuffers_, NdbProfile_lud16, NdbProfile)
__flatbuffers_build_string_field(4, flatbuffers_, NdbProfile_banner, NdbProfile)
__flatbuffers_build_string_field(5, flatbuffers_, NdbProfile_display_name, NdbProfile)
__flatbuffers_build_scalar_field(6, flatbuffers_, NdbProfile_reactions, flatbuffers_bool, flatbuffers_bool_t, 1, 1, UINT8_C(1), NdbProfile)
__flatbuffers_build_string_field(7, flatbuffers_, NdbProfile_picture, NdbProfile)
__flatbuffers_build_string_field(8, flatbuffers_, NdbProfile_nip05, NdbProfile)
__flatbuffers_build_scalar_field(9, flatbuffers_, NdbProfile_damus_donation, flatbuffers_int32, int32_t, 4, 4, INT32_C(0), NdbProfile)
__flatbuffers_build_scalar_field(10, flatbuffers_, NdbProfile_damus_donation_v2, flatbuffers_int32, int32_t, 4, 4, INT32_C(0), NdbProfile)
__flatbuffers_build_string_field(11, flatbuffers_, NdbProfile_lud06, NdbProfile)

static inline NdbProfile_ref_t NdbProfile_create(flatbuffers_builder_t *B __NdbProfile_formal_args)
{
    if (NdbProfile_start(B)
        || NdbProfile_name_add(B, v0)
        || NdbProfile_website_add(B, v1)
        || NdbProfile_about_add(B, v2)
        || NdbProfile_lud16_add(B, v3)
        || NdbProfile_banner_add(B, v4)
        || NdbProfile_display_name_add(B, v5)
        || NdbProfile_picture_add(B, v7)
        || NdbProfile_nip05_add(B, v8)
        || NdbProfile_damus_donation_add(B, v9)
        || NdbProfile_damus_donation_v2_add(B, v10)
        || NdbProfile_lud06_add(B, v11)
        || NdbProfile_reactions_add(B, v6)) {
        return 0;
    }
    return NdbProfile_end(B);
}

static NdbProfile_ref_t NdbProfile_clone(flatbuffers_builder_t *B, NdbProfile_table_t t)
{
    __flatbuffers_memoize_begin(B, t);
    if (NdbProfile_start(B)
        || NdbProfile_name_pick(B, t)
        || NdbProfile_website_pick(B, t)
        || NdbProfile_about_pick(B, t)
        || NdbProfile_lud16_pick(B, t)
        || NdbProfile_banner_pick(B, t)
        || NdbProfile_display_name_pick(B, t)
        || NdbProfile_picture_pick(B, t)
        || NdbProfile_nip05_pick(B, t)
        || NdbProfile_damus_donation_pick(B, t)
        || NdbProfile_damus_donation_v2_pick(B, t)
        || NdbProfile_lud06_pick(B, t)
        || NdbProfile_reactions_pick(B, t)) {
        return 0;
    }
    __flatbuffers_memoize_end(B, t, NdbProfile_end(B));
}

#include "flatcc/flatcc_epilogue.h"
#endif /* PROFILE_BUILDER_H */
