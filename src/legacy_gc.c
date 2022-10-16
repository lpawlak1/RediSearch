
#define RS_GC_C_

#include "inverted_index.h"
#include "redis_index.h"
#include "gc.h"
#include "redismodule.h"
#include "default_gc.h"
#include "numeric_index.h"
#include "tag_index.h"
#include "config.h"

#include "rmutil/util.h"
#include "time_sample.h"

#include <math.h>
#include <sys/param.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <assert.h>

///////////////////////////////////////////////////////////////////////////////////////////////

// convert a frequency to timespec
struct timespec hzToTimeSpec(float hz) {
  struct timespec ret;
  ret.tv_sec = (time_t)floor(1.0 / hz);
  ret.tv_nsec = (long)floor(1000000000.0 / hz) % 1000000000L;
  return ret;
}

#define NUMERIC_GC_INITIAL_SIZE 4

#define SPEC_STATUS_OK 1
#define SPEC_STATUS_INVALID 2

//---------------------------------------------------------------------------------------------

// Create a new garbage collector, with a string for the index name, and initial frequency
GarbageCollector::GarbageCollector(const RedisModuleString *k, float initialHZ, uint64_t specUniqueId) {
  hz = initialHZ;
  keyName = k;
  stats = {0};
  rdbPossiblyLoading = 1;
  noLockMode = false;
  specUniqueId = specUniqueId;
  numericGC = array_new(NumericFieldGC *, NUMERIC_GC_INITIAL_SIZE);
}

//---------------------------------------------------------------------------------------------

void GarbageCollector::updateStats(RedisSearchCtx *sctx, size_t recordsRemoved, size_t bytesCollected) {
  sctx->spec->stats.numRecords -= recordsRemoved;
  sctx->spec->stats.invertedSize -= bytesCollected;
  stats.totalCollected += bytesCollected;
}

//---------------------------------------------------------------------------------------------

size_t GarbageCollector::CollectRandomTerm(RedisModuleCtx *ctx, int *status) {
  RedisModuleKey *idxKey = NULL;
  RedisSearchCtx *sctx = new RedisSearchCtx(ctx, (RedisModuleString *)keyName, false);
  size_t totalRemoved = 0;
  size_t totalCollected = 0;
  TimeSample ts;
  char *term;
  InvertedIndex *idx = NULL;

  if (!sctx || sctx->spec->uniqueId != specUniqueId) {
    RedisModule_Log(ctx, "warning", "No index spec for GC %s",
                    RedisModule_StringPtrLen(keyName, NULL));
    *status = SPEC_STATUS_INVALID;
    goto end;
  }
  // Select a weighted random term
  term = sctx->spec->GetRandomTerm(20);
  // if the index is empty we won't get anything here
  if (!term) {
    goto end;
  }
  RedisModule_Log(ctx, "debug", "Garbage collecting for term '%s'", term);
  // Open the term's index
  idx = Redis_OpenInvertedIndexEx(sctx, term, strlen(term), 1, &idxKey);
  if (idx) {
    int blockNum = 0;
    do {
      IndexBlockRepair params;
      params.limit = RSGlobalConfig.gcScanSize;
      ts.Start();
      // repair 100 blocks at once
      blockNum = idx->Repair(&sctx->spec->docs, blockNum, params);
      ts.End();
      RedisModule_Log(ctx, "debug", "Repair took %lldns", ts.DurationNS());
      /// update the statistics with the the number of records deleted
      totalRemoved += params.docsCollected;
      updateStats(sctx, params.docsCollected, params.bytesCollected);
      totalCollected += params.bytesCollected;
      // blockNum 0 means error or we've finished
      if (!blockNum) break;

      // After each iteration we yield execution
      // First we close the relevant keys we're touching
      RedisModule_CloseKey(idxKey);
      sctx->Refresh((RedisModuleString *)keyName);
      // sctx null --> means it was deleted and we need to stop right now
      if (!sctx || sctx->spec->uniqueId != specUniqueId) {
        *status = SPEC_STATUS_INVALID;
        break;
      }

      // reopen the inverted index - it might have gone away
      idx = Redis_OpenInvertedIndexEx(sctx, term, strlen(term), 1, &idxKey);
    } while (idx != NULL);
  }
  if (totalRemoved) {
    RedisModule_Log(ctx, "debug", "Garbage collected %zd bytes in %zd records for term '%s'",
                    totalCollected, totalRemoved, term);
  }
  rm_free(term);
  RedisModule_Log(ctx, "debug", "New HZ: %f\n", hz);
end:
  if (sctx) {
    delete sctx;
  }
  if (idxKey) RedisModule_CloseKey(idxKey);

  return totalRemoved;
}

//---------------------------------------------------------------------------------------------

NumericRangeNode *NextGcNode(NumericFieldGC *numericGc) {
  bool runFromStart = false;
  NumericRangeNode *node = NULL;
  for (;;) {
    while (node = numericGc->gcIterator->Next()) {
      if (node->range) {
        return node;
      }
    }
    if (runFromStart) throw Error("Second iterator should return result");
    delete numericGc->gcIterator;
    numericGc->gcIterator = new NumericRangeTreeIterator(numericGc->rt);
    runFromStart = true;
  }

  // will never reach here
  return NULL;
}

//---------------------------------------------------------------------------------------------

NumericFieldGC::NumericFieldGC(NumericRangeTree *rt) : rt(rt), revisionId(rt->revisionId){
  gcIterator = new NumericRangeTreeIterator(rt);
}

//---------------------------------------------------------------------------------------------

NumericFieldGC::~NumericFieldGC() {
  delete gcIterator;
}

//---------------------------------------------------------------------------------------------

void GarbageCollector::FreeNumericGCArray() {
  for (int i = 0; i < array_len(numericGC); ++i) {
    delete numericGC[i];
  }
  array_trimm_len(numericGC, 0);
}

//---------------------------------------------------------------------------------------------

static RedisModuleString *getRandomFieldByType(IndexSpec *spec, FieldType type) {
  Vector<FieldSpec> tagFields = spec->getFieldsByType(type);
  if (tagFields.empty()) {
    return NULL;
  }

  // choose random tag field
  int randomIndex = rand() % tagFields.size();

  RedisModuleString *ret = spec->GetFormattedKey(tagFields[randomIndex], type);
  return ret;
}

//---------------------------------------------------------------------------------------------

size_t GarbageCollector::CollectTagIndex(RedisModuleCtx *ctx, int *status) {
  RedisSearchCtx *sctx = new RedisSearchCtx(ctx, (RedisModuleString *)keyName, false);
  RedisModuleString *keyName = NULL;
  TagIndex *indexTag = NULL;
  IndexSpec *spec = NULL;
  RedisModuleKey *idxKey = NULL;
  InvertedIndex *iv;
  std::string randomKey;
  size_t totalRemoved = 0;
  int blockNum = 0;

  if (!sctx || sctx->spec->uniqueId != specUniqueId) {
    RedisModule_Log(ctx, "warning", "No index spec for GC %s", RedisModule_StringPtrLen(keyName, NULL));
    *status = SPEC_STATUS_INVALID;
    goto end;
  }

  spec = sctx->spec;
  keyName = getRandomFieldByType(spec, INDEXFLD_T_TAG);
  if (!keyName) {
    goto end;
  }

  indexTag = TagIndex::Open(sctx, keyName, false, &idxKey);
  if (!indexTag) {
    goto end;
  }

  if (!indexTag->values->RandomKey(randomKey, (void **)&iv)) {
    goto end;
  }

  for (;;) {
    // repair 100 blocks at once
    IndexBlockRepair params;
    params.limit = RSGlobalConfig.gcScanSize;
    blockNum = iv->Repair(&sctx->spec->docs, blockNum, params);
    /// update the statistics with the the number of records deleted
    totalRemoved += params.docsCollected;
    updateStats(sctx, params.docsCollected, params.bytesCollected);
    // blockNum 0 means error or we've finished
    if (!blockNum) break;

    // After each iteration we yield execution
    // First we close the relevant keys we're touching
    RedisModule_CloseKey(idxKey);
    sctx->Refresh((RedisModuleString *)keyName);
    // sctx null --> means it was deleted and we need to stop right now
    if (!sctx || sctx->spec->uniqueId != specUniqueId) {
      *status = SPEC_STATUS_INVALID;
      break;
    }

    // reopen inverted index
    indexTag = TagIndex::Open(sctx, keyName, false, &idxKey);
    if (!indexTag) {
      break;
    }
    iv = indexTag->values->Find(randomKey);
    if (iv == TRIEMAP_NOTFOUND) {
      break;
    }
  }

end:
  if (idxKey) RedisModule_CloseKey(idxKey);

  if (sctx) {
    delete sctx;
  }

  return totalRemoved;
}

//---------------------------------------------------------------------------------------------

size_t GarbageCollector::CollectNumericIndex(RedisModuleCtx *ctx, int *status) {
  size_t totalRemoved = 0;
  RedisModuleKey *idxKey = NULL;
  Vector<FieldSpec> numericFields;
  RedisSearchCtx *sctx = new RedisSearchCtx(ctx, (RedisModuleString *)keyName, false);
  IndexSpec *spec = NULL;
  int randomIndex;
  NumericFieldGC *num_gc = NULL;
  RedisModuleString *keyName = NULL;
  NumericRangeTree *rt = NULL;
  NumericRangeNode *nextNode = NULL;
  int blockNum = 0;

  if (!sctx || sctx->spec->uniqueId != specUniqueId) {
    RedisModule_Log(ctx, "warning", "No index spec for GC %s",
                    RedisModule_StringPtrLen(keyName, NULL));
    *status = SPEC_STATUS_INVALID;
    goto end;
  }

  spec = sctx->spec;
  numericFields = spec->getFieldsByType(INDEXFLD_T_NUMERIC);  // find all the numeric fields
  if (numericFields.empty()) {
    goto end;
  }

  if (numericFields.size() != array_len(numericGC)) {
    // add all numeric fields to our gc
    if (numericFields.size() <= array_len(numericGC)) throw Error("it is not possible to remove fields");
    FreeNumericGCArray();
    for (int i = 0; i < numericFields.size(); ++i) {
      RedisModuleString *keyName = spec->GetFormattedKey(numericFields[i], INDEXFLD_T_NUMERIC);
      NumericRangeTree *rt = OpenNumericIndex(sctx, keyName, &idxKey);
      // if we could not open the numeric field we probably have a
      // corruption in our data, better to know it now.
      if (!rt) throw Error("numeric index failed to open");
      numericGC = array_append(numericGC, new NumericFieldGC(rt));
      if (idxKey) RedisModule_CloseKey(idxKey);
    }
  }

  // choose random numeric gc ctx
  randomIndex = rand() % array_len(numericGC);
  num_gc = numericGC[randomIndex];

  // open the relevent numeric index to check that our pointer is valid
  keyName = spec->GetFormattedKey(numericFields[randomIndex], INDEXFLD_T_NUMERIC);
  rt = OpenNumericIndex(sctx, keyName, &idxKey);
  if (idxKey) RedisModule_CloseKey(idxKey);

  if (num_gc->rt != rt || num_gc->revisionId != num_gc->rt->revisionId) {
    // memory or revision changed, recreating our numeric gc ctx
    if (num_gc->rt == rt && num_gc->revisionId >= num_gc->rt->revisionId) {
      throw Error("NumericRangeTree or revisionId are inncorrect");
    }
    numericGC[randomIndex] = new NumericFieldGC(rt);
    delete num_gc;
    num_gc = numericGC[randomIndex];
  }

  nextNode = NextGcNode(num_gc);
  for (;;) {
    IndexBlockRepair params;// = { limit: RSGlobalConfig.gcScanSize, arg: nextNode->range };
    params.limit = RSGlobalConfig.gcScanSize;
    // repair 100 blocks at once
    blockNum = nextNode->range->entries.Repair(&sctx->spec->docs, blockNum, params);
    /// update the statistics with the the number of records deleted
    num_gc->rt->numEntries -= params.docsCollected;
    totalRemoved += params.docsCollected;
    updateStats(sctx, params.docsCollected, params.bytesCollected);
    // blockNum 0 means error or we've finished
    if (!blockNum) break;

    sctx->Refresh((RedisModuleString *)keyName);
    // sctx null --> means it was deleted and we need to stop right now
    if (!sctx || sctx->spec->uniqueId != specUniqueId) {
      *status = SPEC_STATUS_INVALID;
      break;
    }
    if (num_gc->revisionId != num_gc->rt->revisionId) {
      break;
    }
  }

end:
  if (sctx) {
    delete sctx;
  }

  return totalRemoved;
}

//---------------------------------------------------------------------------------------------

// The GC periodic callback, called in a separate thread.
// It selects a random term (using weighted random).

bool GarbageCollector::PeriodicCallback(RedisModuleCtx *ctx) {
  int status = SPEC_STATUS_OK;
  RedisModule_AutoMemory(ctx);
  RedisModule_ThreadSafeContextLock(ctx);
  size_t totalRemoved = 0;

  // Check if RDB is loading - not needed after the first time we find out that rdb is not reloading
  if (rdbPossiblyLoading) {
    if (isRdbLoading(ctx)) {
      RedisModule_Log(ctx, "notice", "RDB Loading in progress, not performing GC");
      goto end;
    } else {
      // the RDB will not load again, so it's safe to ignore the info check in the next cycles
      rdbPossiblyLoading = 0;
    }
  }

  totalRemoved += CollectRandomTerm(ctx, &status);
  totalRemoved += CollectNumericIndex(ctx, &status);
  totalRemoved += CollectTagIndex(ctx, &status);

  stats.numCycles++;
  stats.effectiveCycles += totalRemoved > 0 ? 1 : 0;

  // if we didn't remove anything - reduce the frequency a bit.
  // if we did  - increase the frequency a bit
  // the timer is NULL if we've been cancelled
  if (totalRemoved > 0) {
    hz = MIN(hz * 1.2, GC_MAX_HZ);
  } else {
    hz = MAX(hz * 0.99, GC_MIN_HZ);
  }

end:
  RedisModule_ThreadSafeContextUnlock(ctx);

  return status == SPEC_STATUS_OK;
}

//---------------------------------------------------------------------------------------------

// Termination callback for the GC. Called after we stop, and frees up all the resources

void GarbageCollector::OnTerm() {
  RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
  RedisModule_ThreadSafeContextLock(ctx);
  RedisModule_FreeString(ctx, (RedisModuleString *)keyName);
  for (int i = 0; i < array_len(numericGC); ++i) {
    delete numericGC[i];
  }
  array_free(numericGC);
  RedisModule_ThreadSafeContextUnlock(ctx);
  RedisModule_FreeThreadSafeContext(ctx);
  //@@ delete this; // GC::~GC does this
}

//---------------------------------------------------------------------------------------------

// called externally when the user deletes a document to hint at increasing the HZ

void GarbageCollector::OnDelete() {
  hz = MIN(hz * 1.5, GC_MAX_HZ);
}

//---------------------------------------------------------------------------------------------

struct timespec GarbageCollector::GetInterval() {
  return hzToTimeSpec(hz);
}

//---------------------------------------------------------------------------------------------

// Render the GC stats to a redis connection, used by FT.INFO

void GarbageCollector::RenderStats(RedisModuleCtx *ctx) {
#define REPLY_KVNUM(n, k, v)                   \
  RedisModule_ReplyWithSimpleString(ctx, k);   \
  RedisModule_ReplyWithDouble(ctx, (double)v); \
  n += 2

  int n = 0;
  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

  REPLY_KVNUM(n, "current_hz", hz);
  REPLY_KVNUM(n, "bytes_collected", stats.totalCollected);
  REPLY_KVNUM(n, "effectiv_cycles_rate",
              (double)stats.effectiveCycles /
              (double)(stats.numCycles ? stats.numCycles : 1));

  RedisModule_ReplySetArrayLength(ctx, n);
}

///////////////////////////////////////////////////////////////////////////////////////////////
