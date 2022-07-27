#pragma once

#include "trie/trie.h"
#include "trie/levenshtein.h"

//#include "redismodule.h"

///////////////////////////////////////////////////////////////////////////////////////////////

extern RedisModuleType *TrieType;

#define TRIE_ENCVER_CURRENT 1
#define TRIE_ENCVER_NOPAYLOADS 0

struct TrieSearchResult;

//---------------------------------------------------------------------------------------------

struct Trie {
  TrieNode *root;
  size_t size;

  Trie();
  ~Trie();

  bool Delete(const char *s);
  int Insert(RedisModuleString *s, double score, int incr, RSPayload *payload);
  int InsertStringBuffer(const char *s, size_t len, double score, int incr, RSPayload *payload);
  Vector<TrieSearchResult*> Search(const char *s, size_t len, size_t num, int maxDist, int prefixMode, int trim, int optimize);

  TrieIterator<DFAFilter> Iterate(const char *prefix, size_t len, int maxDist, int prefixMode);

  bool RandomKey(char **str, t_len *len, double *score);
};

//---------------------------------------------------------------------------------------------

struct TrieSearchResult : Object {
  char *str;
  size_t len;
  float score;
  char *payload;
  size_t plen;

  ~TrieSearchResult();
};

//---------------------------------------------------------------------------------------------

#define SCORE_TRIM_FACTOR 10.0

// Commands related to the redis TrieType registration
int TrieType_Register(RedisModuleCtx *ctx);
void *TrieType_GenericLoad(RedisModuleIO *rdb, int loadPayloads);
void TrieType_GenericSave(RedisModuleIO *rdb, Trie *t, int savePayloads);
void *TrieType_RdbLoad(RedisModuleIO *rdb, int encver);
void TrieType_RdbSave(RedisModuleIO *rdb, void *value);
void TrieType_Digest(RedisModuleDigest *digest, void *value);
void TrieType_Free(void *value);

///////////////////////////////////////////////////////////////////////////////////////////////
