#pragma once

#include <iowow/basedefs.h>
#include <iowow/iwpool.h>

IW_EXTERN_C_START

struct iwn_pairs;

struct iwn_val {
  size_t len;
  char  *buf;
  struct iwn_val *next;
};

struct iwn_vals {
  struct iwn_val *first;
  struct iwn_val *last;
};

struct iwn_pair {
  const char *key;
  char       *val;
  size_t      key_len;
  size_t      val_len;
  struct iwn_pair  *next;  ///< Next pair in pairs chain
  struct iwn_pairs *extra; ///< Optional extra pairs associated with this pair
};

struct iwn_pairs {
  struct iwn_pair *first;
  struct iwn_pair *last;
};

IW_EXPORT void iwn_val_buf_free(struct iwn_val *val);

IW_EXPORT void iwn_val_add(struct iwn_vals *vals, struct iwn_val *v);

IW_EXPORT iwrc iwn_val_add_new(struct iwn_vals *vals, char *buf, size_t len);

IW_EXPORT void iwn_pair_add(struct iwn_pairs *pairs, struct iwn_pair *p);

IW_EXPORT struct iwn_pair* iwn_pair_find(struct iwn_pairs *pairs, const char *key, ssize_t key_len);

IW_EXPORT struct iwn_val iwn_pair_find_val(struct iwn_pairs *pairs, const char *key, ssize_t key_len);

IW_EXPORT iwrc iwn_pair_add_pool(
  IWPOOL           *pool,
  struct iwn_pairs *pairs,
  const char       *key,
  ssize_t           key_len,
  char             *val,
  ssize_t           val_len);


IW_EXTERN_C_END