#include "BTreeVI.hpp"

#include "leanstore/concurrency-recovery/CRMG.hpp"
// -------------------------------------------------------------------------------------
#include "gflags/gflags.h"
// -------------------------------------------------------------------------------------
#include <signal.h>
// -------------------------------------------------------------------------------------
using namespace std;
using namespace leanstore::storage;
using OP_RESULT = leanstore::OP_RESULT;
// -------------------------------------------------------------------------------------
// Assumptions made in this implementation:
// 1) We don't insert an already removed key
// 2) Secondary Versions contain delta
// Keep in mind that garbage collection may leave pages completely empty
// Missing points: FatTuple::remove, garbage leaves can escape from us
namespace leanstore
{
namespace storage
{
namespace btree
{
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookup(u8* o_key, u16 o_key_length, function<void(const u8*, u16)> payload_callback)
{
   if (cr::activeTX().isSerializable()) {
      return lookupPessimistic(o_key, o_key_length, payload_callback);
   }
   const OP_RESULT ret = lookupOptimistic(o_key, o_key_length, payload_callback);
   if (ret == OP_RESULT::OTHER) {
      return lookupPessimistic(o_key, o_key_length, payload_callback);
   } else {
      return ret;
   }
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookupPessimistic(u8* key_buffer, const u16 key_length, function<void(const u8*, u16)> payload_callback)
{
   MutableSlice m_key(key_buffer, key_length);
   Slice key(key_buffer, key_length);
   jumpmuTry()
   {
      BTreeSharedIterator iterator(*static_cast<BTreeGeneric*>(this),
                                   cr::activeTX().isSerializable() ? LATCH_FALLBACK_MODE::EXCLUSIVE : LATCH_FALLBACK_MODE::SHARED);
      auto ret = iterator.seekExact(key);
      explainIfNot(ret == OP_RESULT::OK);
      if (ret != OP_RESULT::OK) {
         jumpmu_return OP_RESULT::NOT_FOUND;
      }
      [[maybe_unused]] const auto primary_version = *reinterpret_cast<const ChainedTuple*>(iterator.value().data());
      auto reconstruct = reconstructTuple(iterator, [&](Slice value) { payload_callback(value.data(), value.length()); });
      COUNTERS_BLOCK()
      {
         WorkerCounters::myCounters().cc_read_chains[dt_id]++;
         WorkerCounters::myCounters().cc_read_versions_visited[dt_id] += std::get<1>(reconstruct);
      }
      ret = std::get<0>(reconstruct);
      if (ret != OP_RESULT::ABORT_TX && ret != OP_RESULT::OK) {  // For debugging
         cout << endl;
         cout << u64(std::get<1>(reconstruct)) << endl;
         raise(SIGTRAP);
      }
      jumpmu_return ret;
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::lookupOptimistic(const u8* key, const u16 key_length, function<void(const u8*, u16)> payload_callback)
{
   while (true) {
      jumpmuTry()
      {
         HybridPageGuard<BTreeNode> leaf;
         findLeafCanJump(leaf, key, key_length);
         // -------------------------------------------------------------------------------------
         s16 pos = leaf->lowerBound<true>(key, key_length);
         if (pos != -1) {
            auto& tuple = *reinterpret_cast<Tuple*>(leaf->getPayload(pos));
            if (isVisibleForMe(tuple.worker_id, tuple.tx_id, false)) {
               u32 offset = 0;
               if (tuple.tuple_format == TupleFormat::CHAINED) {
                  offset = sizeof(ChainedTuple);
               } else if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
                  offset = sizeof(FatTupleDifferentAttributes);
               } else {
                  leaf.recheck();
                  UNREACHABLE();
               }
               payload_callback(leaf->getPayload(pos) + offset, leaf->getPayloadLength(pos) - offset);
               leaf.recheck();
               COUNTERS_BLOCK()
               {
                  WorkerCounters::myCounters().cc_read_chains[dt_id]++;
                  WorkerCounters::myCounters().cc_read_versions_visited[dt_id] += 1;
               }
               jumpmu_return OP_RESULT::OK;
            } else {
               jumpmu_break;
            }
         } else {
            leaf.recheck();
            raise(SIGTRAP);
            jumpmu_return OP_RESULT::NOT_FOUND;
         }
      }
      jumpmuCatch() {}
   }
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::updateSameSizeInPlace(u8* o_key,
                                         u16 o_key_length,
                                         function<void(u8* value, u16 value_size)> callback,
                                         UpdateSameSizeInPlaceDescriptor& update_descriptor)
{
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   Slice key(o_key, o_key_length);
   OP_RESULT ret;
   // -------------------------------------------------------------------------------------
   // 20K instructions more
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
      ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {
         raise(SIGTRAP);
         jumpmu_return ret;
      }
      // -------------------------------------------------------------------------------------
   restart : {
      MutableSlice primary_payload = iterator.mutableValue();
      auto& tuple = *reinterpret_cast<Tuple*>(primary_payload.data());
      if (tuple.isWriteLocked() || !isVisibleForMe(tuple.worker_id, tuple.tx_id, true)) {
         jumpmu_return OP_RESULT::ABORT_TX;
      }
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            if (tuple.read_lock_counter > 0 && tuple.read_lock_counter != (1ull << cr::Worker::my().workerID())) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         } else {
            if (tuple.read_ts > cr::activeTX().TTS()) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         }
      }
      tuple.writeLock();
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_chains[dt_id]++; }
      // -------------------------------------------------------------------------------------
      if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
         const bool res =
             reinterpret_cast<FatTupleDifferentAttributes*>(&tuple)->update(iterator, o_key, o_key_length, callback, update_descriptor, *this);
         ensure(res);  // TODO: what if it fails, then we have to do something else
         // Attention: tuple pointer is not valid here
         reinterpret_cast<Tuple*>(iterator.mutableValue().data())->unlock();
         // -------------------------------------------------------------------------------------
         if (cr::activeTX().isSingleStatement()) {
            cr::Worker::my().commitTX();
         }
         // -------------------------------------------------------------------------------------
         iterator.contentionSplit();
         // -------------------------------------------------------------------------------------
         jumpmu_return OP_RESULT::OK;
      }
      // -------------------------------------------------------------------------------------
      // TODO:
      // bool convert_to_fat_tuple = FLAGS_vi_fat_tuple && chain_head.can_convert_to_fat_tuple &&
      //                             !(chain_head.worker_id == cr::Worker::my().workerID() && chain_head.worker_txid == cr::activeTX().TTS());
      // if (convert_to_fat_tuple) {
      //    // const u64 random_number = utils::RandomGenerator::getRandU64();
      //    // convert_to_fat_tuple &= ((random_number & ((1ull << FLAGS_vi_fat_tuple_threshold) - 1)) == 0);
      //    convert_to_fat_tuple &= cr::Worker::my().global_snapshot_lwm.load() < chain_head.worker_txid;
      // }
      // if (convert_to_fat_tuple) {
      //    ensure(chain_head.isWriteLocked());
      //    const bool convert_ret = convertChainedToFatTupleDifferentAttributes(iterator, m_key);
      //    if (convert_ret) {
      //       COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_fat_tuple_convert[dt_id]++; }
      //    }
      //    goto restart;
      //    UNREACHABLE();
      // }
      // -------------------------------------------------------------------------------------
   }
      bool update_without_versioning = (FLAGS_vi_update_version_elision || !FLAGS_mv || FLAGS_vi_fupdate_chained);
      if (update_without_versioning && !FLAGS_vi_fupdate_chained && FLAGS_vi_update_version_elision) {
         // Avoid creating version if all transactions are running in read-committed mode and the current tx is single-statement
         update_without_versioning &= cr::activeTX().isSingleStatement();
         for (u64 w_i = 0; w_i < cr::Worker::my().workers_count && update_without_versioning; w_i++) {
            update_without_versioning &= (cr::Worker::my().global_workers_in_progress_txid[w_i].load() & (1ull << 63));
         }
      }
      // -------------------------------------------------------------------------------------
      // Update in chained mode
      MutableSlice primary_payload = iterator.mutableValue();
      auto& tuple_head = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
      const u16 delta_and_descriptor_size = update_descriptor.size() + update_descriptor.diffLength();
      const u16 secondary_payload_length = delta_and_descriptor_size + sizeof(ChainedTupleVersion);
      const ChainSN command_id = cr::Worker::my().command_id++;
      // -------------------------------------------------------------------------------------
      // Write the ChainedTupleDelta
      if (!update_without_versioning) {
         cr::Worker::my().versions_space.insertVersion(cr::activeTX().TTS(), dt_id, command_id, secondary_payload_length, [&](u8* version_payload) {
            auto& secondary_version =
                *new (version_payload) ChainedTupleVersion(tuple_head.worker_id, tuple_head.tx_id, false, true, cr::activeTX().TTS());
            std::memcpy(secondary_version.payload, &update_descriptor, update_descriptor.size());
            BTreeLL::generateDiff(update_descriptor, secondary_version.payload + update_descriptor.size(), tuple_head.payload);
            secondary_version.command_id = tuple_head.command_id;
            if (secondary_version.worker_id == cr::Worker::my().workerID() && secondary_version.tx_id == cr::activeTX().TTS()) {
               secondary_version.committed_before_txid = std::numeric_limits<u64>::max();
            } else {
               secondary_version.committed_before_txid = cr::activeTX().TTS();
            }
         });
         COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_update_versions_created[dt_id]++; }
      }
      // -------------------------------------------------------------------------------------
      iterator.markAsDirty();
      // -------------------------------------------------------------------------------------
      // WAL
      auto wal_entry = iterator.leaf.reserveWALEntry<WALUpdateSSIP>(o_key_length + delta_and_descriptor_size);
      wal_entry->type = WAL_LOG_TYPE::WALUpdate;
      wal_entry->key_length = o_key_length;
      wal_entry->delta_length = delta_and_descriptor_size;
      wal_entry->before_worker_id = tuple_head.worker_id;
      wal_entry->before_tx_id = tuple_head.tx_id;
      wal_entry->before_command_id = tuple_head.command_id;
      wal_entry->after_worker_id = cr::Worker::my().workerID();
      wal_entry->after_tx_id = cr::activeTX().TTS();
      wal_entry->after_command_id = command_id;
      std::memcpy(wal_entry->payload, o_key, o_key_length);
      std::memcpy(wal_entry->payload + o_key_length, &update_descriptor, update_descriptor.size());
      BTreeLL::generateDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), tuple_head.payload);
      callback(tuple_head.payload, primary_payload.length() - sizeof(ChainedTuple));  // Update
      BTreeLL::generateXORDiff(update_descriptor, wal_entry->payload + o_key_length + update_descriptor.size(), tuple_head.payload);
      wal_entry.submit();
      // -------------------------------------------------------------------------------------
      tuple_head.worker_id = cr::Worker::my().workerID();
      tuple_head.tx_id = cr::activeTX().TTS();
      tuple_head.command_id = command_id;
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            // Nothing, the WorkerID + Commit HWM are the write lock
            tuple_head.read_lock_counter = (1ull << cr::Worker::my().workerID());
         } else {
            tuple_head.read_ts = cr::activeTX().TTS();
         }
      }
      // -------------------------------------------------------------------------------------
      tuple_head.unlock();
      iterator.contentionSplit();
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isSingleStatement()) {
         cr::Worker::my().commitTX();
      }
      // -------------------------------------------------------------------------------------
      jumpmu_return OP_RESULT::OK;
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::insert(u8* o_key, u16 o_key_length, u8* value, u16 value_length)
{
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   Slice key(o_key, o_key_length);
   const u16 payload_length = value_length + sizeof(ChainedTuple);
   // -------------------------------------------------------------------------------------
   while (true) {
      jumpmuTry()
      {
         BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
         OP_RESULT ret = iterator.seekToInsert(key);
         if (ret == OP_RESULT::DUPLICATE) {
            MutableSlice primary_payload = iterator.mutableValue();
            auto& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
            if (primary_version.isWriteLocked() || !isVisibleForMe(primary_version.worker_id, primary_version.tx_id, true)) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
            ensure(false);  // Not implemented: maybe it has been removed but no GCed
         }
         ret = iterator.enoughSpaceInCurrentNode(key, payload_length);
         if (ret == OP_RESULT::NOT_ENOUGH_SPACE) {
            iterator.splitForKey(key);
            jumpmu_continue;
         }
         // -------------------------------------------------------------------------------------
         // WAL
         auto wal_entry = iterator.leaf.reserveWALEntry<WALInsert>(o_key_length + value_length);
         wal_entry->type = WAL_LOG_TYPE::WALInsert;
         wal_entry->key_length = o_key_length;
         wal_entry->value_length = value_length;
         std::memcpy(wal_entry->payload, o_key, o_key_length);
         std::memcpy(wal_entry->payload + o_key_length, value, value_length);
         wal_entry.submit();
         // -------------------------------------------------------------------------------------
         iterator.insertInCurrentNode(key, payload_length);
         MutableSlice payload = iterator.mutableValue();
         auto& primary_version = *new (payload.data()) ChainedTuple(cr::Worker::my().workerID(), cr::activeTX().TTS());
         std::memcpy(primary_version.payload, value, value_length);
         // -------------------------------------------------------------------------------------
         if (cr::activeTX().isSingleStatement()) {
            cr::Worker::my().commitTX();
         }
         // -------------------------------------------------------------------------------------
         jumpmu_return OP_RESULT::OK;
      }
      jumpmuCatch() { UNREACHABLE(); }
   }
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::remove(u8* o_key, u16 o_key_length)
{
   // TODO: remove fat tuple
   assert(!cr::activeTX().isReadOnly());
   cr::Worker::my().walEnsureEnoughSpace(PAGE_SIZE * 1);
   Slice key(o_key, o_key_length);
   // -------------------------------------------------------------------------------------
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(this));
      OP_RESULT ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {
         explainWhen(cr::activeTX().atLeastSI());
         jumpmu_return OP_RESULT::NOT_FOUND;
      }
      // -------------------------------------------------------------------------------------
      if (FLAGS_vi_fremove) {
         ret = iterator.removeCurrent();
         ensure(ret == OP_RESULT::OK);
         iterator.mergeIfNeeded();
         jumpmu_return OP_RESULT::OK;
      }
      // -------------------------------------------------------------------------------------
      const u64 command_id = cr::Worker::my().command_id++;
      // -------------------------------------------------------------------------------------
      auto payload = iterator.mutableValue();
      ChainedTuple& tuple_head = *reinterpret_cast<ChainedTuple*>(payload.data());
      // -------------------------------------------------------------------------------------
      ensure(tuple_head.tuple_format == TupleFormat::CHAINED);  // TODO: removing fat tuple is not supported atm
      if (tuple_head.isWriteLocked() || !isVisibleForMe(tuple_head.worker_id, tuple_head.tx_id, true)) {
         jumpmu_return OP_RESULT::ABORT_TX;
      }
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            if (tuple_head.read_lock_counter > 0 && tuple_head.read_lock_counter != (1ull << cr::Worker::my().workerID())) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         } else {
            if (tuple_head.read_ts > cr::activeTX().TTS()) {
               jumpmu_return OP_RESULT::ABORT_TX;
            }
         }
      }
      ensure(!cr::activeTX().atLeastSI() || tuple_head.is_removed == false);
      if (tuple_head.is_removed) {
         jumpmu_return OP_RESULT::NOT_FOUND;
      }
      // -------------------------------------------------------------------------------------
      tuple_head.writeLock();
      // -------------------------------------------------------------------------------------
      const u16 value_length = iterator.value().length() - sizeof(ChainedTuple);
      const u16 secondary_payload_length = sizeof(ChainedTupleVersion) + value_length;
      cr::Worker::my().versions_space.insertVersion(cr::activeTX().TTS(), dt_id, command_id, secondary_payload_length, [&](u8* secondary_payload) {
         auto& secondary_version =
             *new (secondary_payload) ChainedTupleVersion(tuple_head.worker_id, tuple_head.tx_id, false, false, cr::activeTX().TTS());
         secondary_version.worker_id = tuple_head.worker_id;
         secondary_version.tx_id = tuple_head.tx_id;
         secondary_version.command_id = tuple_head.command_id;
         std::memcpy(secondary_version.payload, tuple_head.payload, value_length);
      });
      iterator.markAsDirty();
      DanglingPointer dangling_pointer;
      dangling_pointer.bf = iterator.leaf.bf;
      dangling_pointer.latch_version_should_be = iterator.leaf.guard.version;
      dangling_pointer.head_slot = iterator.cur;
      // -------------------------------------------------------------------------------------
      // WAL
      auto wal_entry = iterator.leaf.reserveWALEntry<WALRemove>(o_key_length + value_length);
      wal_entry->type = WAL_LOG_TYPE::WALRemove;
      wal_entry->key_length = o_key_length;
      wal_entry->value_length = value_length;
      wal_entry->before_worker_id = tuple_head.worker_id;
      wal_entry->before_tx_id = tuple_head.tx_id;
      wal_entry->before_command_id = tuple_head.command_id;
      std::memcpy(wal_entry->payload, o_key, o_key_length);
      std::memcpy(wal_entry->payload + o_key_length, tuple_head.payload, value_length);
      wal_entry.submit();
      // -------------------------------------------------------------------------------------
      if (payload.length() - sizeof(ChainedTuple) > 1) {
         iterator.shorten(sizeof(ChainedTuple));
      }
      tuple_head.is_removed = true;
      tuple_head.worker_id = cr::Worker::my().workerID();
      tuple_head.tx_id = cr::activeTX().TTS();
      tuple_head.command_id = command_id;
      if (cr::activeTX().isSerializable()) {
         if (FLAGS_2pl) {
            tuple_head.read_lock_counter = (1ull << cr::Worker::my().workerID());
         } else {
            tuple_head.read_ts = cr::activeTX().TTS();
         }
      }
      // -------------------------------------------------------------------------------------
      if (FLAGS_vi_rtodo) {
         cr::Worker::my().stageTODO(cr::Worker::my().workerID(), cr::activeTX().TTS(), dt_id, o_key_length + sizeof(TODOPoint), [&](u8* entry) {
            auto& todo_entry = *new (entry) TODOPoint();
            todo_entry.key_length = o_key_length;
            todo_entry.dangling_pointer = dangling_pointer;
            std::memcpy(todo_entry.key, o_key, o_key_length);
         });
      }
      // -------------------------------------------------------------------------------------
      tuple_head.unlock();
      // -------------------------------------------------------------------------------------
      if (cr::activeTX().isSingleStatement()) {
         cr::Worker::my().commitTX();
      }
      // -------------------------------------------------------------------------------------
      jumpmu_return OP_RESULT::OK;
   }
   jumpmuCatch() {}
   UNREACHABLE();
   return OP_RESULT::OTHER;
}
// -------------------------------------------------------------------------------------
// This undo implementation works only for rollback and not for undo operations during recovery
void BTreeVI::undo(void* btree_object, const u8* wal_entry_ptr, const u64)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   static_cast<void>(btree);
   const WALEntry& entry = *reinterpret_cast<const WALEntry*>(wal_entry_ptr);
   switch (entry.type) {
      case WAL_LOG_TYPE::WALInsert: {  // Assuming no insert after remove
         auto& insert_entry = *reinterpret_cast<const WALInsert*>(&entry);
         jumpmuTry()
         {
            Slice key(insert_entry.payload, insert_entry.key_length);
            // -------------------------------------------------------------------------------------
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            OP_RESULT ret = iterator.seekExact(key);
            ensure(ret == OP_RESULT::OK);
            ret = iterator.removeCurrent();
            ensure(ret == OP_RESULT::OK);
            iterator.markAsDirty();  // TODO: write CLS
            iterator.mergeIfNeeded();
         }
         jumpmuCatch() {}
         break;
      }
      case WAL_LOG_TYPE::WALUpdate: {
         auto& update_entry = *reinterpret_cast<const WALUpdateSSIP*>(&entry);
         jumpmuTry()
         {
            Slice key(update_entry.payload, update_entry.key_length);
            // -------------------------------------------------------------------------------------
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            OP_RESULT ret = iterator.seekExact(key);
            ensure(ret == OP_RESULT::OK);
            auto& tuple = *reinterpret_cast<Tuple*>(iterator.mutableValue().data());
            ensure(!tuple.isWriteLocked());
            if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
               reinterpret_cast<FatTupleDifferentAttributes*>(iterator.mutableValue().data())->undoLastUpdate();
            } else {
               auto& chain_head = *reinterpret_cast<ChainedTuple*>(iterator.mutableValue().data());
               ensure(!chain_head.isWriteLocked());
               ensure(chain_head.tuple_format == TupleFormat::CHAINED);
               // -------------------------------------------------------------------------------------
               chain_head.worker_id = update_entry.before_worker_id;
               chain_head.tx_id = update_entry.before_tx_id;
               chain_head.command_id = update_entry.before_command_id;
               const auto& update_descriptor =
                   *reinterpret_cast<const UpdateSameSizeInPlaceDescriptor*>(update_entry.payload + update_entry.key_length);
               BTreeLL::applyXORDiff(update_descriptor, chain_head.payload,
                                     update_entry.payload + update_entry.key_length + update_descriptor.size());
            }
            // -------------------------------------------------------------------------------------
            jumpmu_return;
         }
         jumpmuCatch() { UNREACHABLE(); }
         break;
      }
      case WAL_LOG_TYPE::WALRemove: {
         auto& remove_entry = *reinterpret_cast<const WALRemove*>(&entry);
         Slice key(remove_entry.payload, remove_entry.key_length);
         // -------------------------------------------------------------------------------------
         jumpmuTry()
         {
            BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
            // -------------------------------------------------------------------------------------
            OP_RESULT ret = iterator.seekExact(key);
            ensure(ret == OP_RESULT::OK);
            // Resize
            const u16 new_primary_payload_length = remove_entry.value_length + sizeof(ChainedTuple);
            const Slice old_primary_payload = iterator.value();
            if (old_primary_payload.length() < new_primary_payload_length) {
               iterator.extendPayload(new_primary_payload_length);
            } else {
               iterator.shorten(new_primary_payload_length);
            }
            MutableSlice primary_payload = iterator.mutableValue();
            auto& primary_version = *new (primary_payload.data()) ChainedTuple(remove_entry.before_worker_id, remove_entry.before_tx_id);
            std::memcpy(primary_version.payload, remove_entry.payload + remove_entry.key_length, remove_entry.value_length);
            primary_version.command_id = remove_entry.before_command_id;
            ensure(primary_version.is_removed == false);
            primary_version.unlock();
            iterator.markAsDirty();
         }
         jumpmuCatch() { UNREACHABLE(); }
         break;
      }
      default: {
         break;
      }
   }
}
// -------------------------------------------------------------------------------------
bool BTreeVI::precisePageWiseGarbageCollection(HybridPageGuard<BTreeNode>& c_guard)
{
   bool all_tuples_heads_are_invisible = true;  // WRT scanners
   u32 garbage_seen_in_bytes = 0;
   u32 freed_bytes = 0;
   for (u16 s_i = 0; s_i < c_guard->count;) {
      auto& sn = *reinterpret_cast<ChainSN*>(c_guard->getKey(s_i) + c_guard->getKeyLen(s_i) - sizeof(ChainSN));
      if (sn == 0) {
         auto& tuple = *reinterpret_cast<Tuple*>(c_guard->getPayload(s_i));
         if (tuple.tuple_format == TupleFormat::CHAINED) {
            auto& chained_tuple = *reinterpret_cast<ChainedTuple*>(c_guard->getPayload(s_i));
            if (chained_tuple.is_removed) {
               all_tuples_heads_are_invisible &= (isVisibleForMe(tuple.worker_id, tuple.tx_id, false));
               const u32 size = c_guard->getKVConsumedSpace(s_i);
               garbage_seen_in_bytes += size;
               if (chained_tuple.tx_id <= cr::Worker::my().global_snapshot_lwm) {
                  c_guard->removeSlot(s_i);
                  freed_bytes += size;
               } else {
                  s_i++;
               }
            } else {
               all_tuples_heads_are_invisible &= !(isVisibleForMe(tuple.worker_id, tuple.tx_id, false));
               s_i++;
            }
         } else if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
            // TODO: Fix FatTuple size
            all_tuples_heads_are_invisible &= !(isVisibleForMe(tuple.worker_id, tuple.tx_id, false));
            s_i++;
         }
      } else {
         auto& chained_tuple_version = *reinterpret_cast<ChainedTupleVersion*>(c_guard->getPayload(s_i));
         const u32 size = c_guard->getKVConsumedSpace(s_i);
         if (chained_tuple_version.gc_trigger <= cr::Worker::my().global_snapshot_lwm) {
            c_guard->removeSlot(s_i);
            freed_bytes += size;
         } else {
            garbage_seen_in_bytes += size;
            s_i++;
         }
      }
   }
   c_guard->gc_space_used = garbage_seen_in_bytes;
   // -------------------------------------------------------------------------------------
   const bool have_we_modified_the_page = (freed_bytes > 0) || (all_tuples_heads_are_invisible);
   if (have_we_modified_the_page) {
      c_guard.incrementGSN();
   }
   return all_tuples_heads_are_invisible;
}
// -------------------------------------------------------------------------------------
SpaceCheckResult BTreeVI::checkSpaceUtilization(void* btree_object, BufferFrame& bf)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   Guard bf_guard(bf.header.latch);
   bf_guard.toOptimisticOrJump();
   HybridPageGuard<BTreeNode> c_guard(std::move(bf_guard), &bf);
   if (!c_guard->is_leaf || !triggerPageWiseGarbageCollection(c_guard)) {
      return BTreeGeneric::checkSpaceUtilization(static_cast<BTreeGeneric*>(&btree), bf);
   }
   // -------------------------------------------------------------------------------------
   bool has_removed_anything = false;
   for (u16 s_i = 0; s_i < c_guard->count;) {
      auto& tuple = *reinterpret_cast<Tuple*>(c_guard->getPayload(s_i));
      if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
         // TODO: Fix FatTuple size
         s_i++;
      }
   }
   if (has_removed_anything) {
      const SpaceCheckResult xmerge_ret = BTreeGeneric::checkSpaceUtilization(static_cast<BTreeGeneric*>(&btree), bf);
      if (xmerge_ret == SpaceCheckResult::PICK_ANOTHER_BF) {
         return SpaceCheckResult::PICK_ANOTHER_BF;
      } else {
         return SpaceCheckResult::RETRY_SAME_BF;
      }
   } else {
      return BTreeGeneric::checkSpaceUtilization(static_cast<BTreeGeneric*>(&btree), bf);
   }
}
// -------------------------------------------------------------------------------------
void BTreeVI::todo(void* btree_object, const u8* entry_ptr, const u64 version_worker_id, const u64 version_tts)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   // Only point-gc
   const TODOPoint& point_todo = *reinterpret_cast<const TODOPoint*>(entry_ptr);
   if (FLAGS_vi_dangling_pointer) {
      // Optimistic fast path
      jumpmuTry()
      {
         BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree), point_todo.dangling_pointer.bf,
                                         point_todo.dangling_pointer.latch_version_should_be);
         assert(point_todo.dangling_pointer.bf != nullptr);
         auto& node = iterator.leaf;
         auto& head = *reinterpret_cast<ChainedTuple*>(node->getPayload(point_todo.dangling_pointer.head_slot));
         // Being chained is implicit because we check for version, so the state can not be changed after staging the todo
         ensure(head.tuple_format == TupleFormat::CHAINED && !head.isWriteLocked());
         ensure(head.worker_id == version_worker_id && head.tx_id == version_tts);
         if (head.is_removed) {
            iterator.leaf->gc_space_used -= iterator.leaf->getKVConsumedSpace(point_todo.dangling_pointer.head_slot);
            node->removeSlot(point_todo.dangling_pointer.head_slot);
         }
         iterator.markAsDirty();
         iterator.mergeIfNeeded();
         jumpmu_return;
      }
      jumpmuCatch() {}
   }
   // -------------------------------------------------------------------------------------
   Slice key(point_todo.key, point_todo.key_length);
   OP_RESULT ret;
   // -------------------------------------------------------------------------------------
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
      ret = iterator.seekExact(key);
      if (ret != OP_RESULT::OK) {  // Legit case
         jumpmu_return;
      }
      COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_todo_chains[btree.dt_id]++; }
      // -------------------------------------------------------------------------------------
      MutableSlice primary_payload = iterator.mutableValue();
      {
         // Checks
         const auto& tuple = *reinterpret_cast<const Tuple*>(primary_payload.data());
         if (tuple.tuple_format == TupleFormat::FAT_TUPLE_DIFFERENT_ATTRIBUTES) {
            jumpmu_return;
         }
      }
      // -------------------------------------------------------------------------------------
      ChainedTuple& primary_version = *reinterpret_cast<ChainedTuple*>(primary_payload.data());
      if (!primary_version.isWriteLocked()) {
         if (primary_version.worker_id == version_worker_id && primary_version.tx_id == version_tts && primary_version.is_removed) {
            iterator.leaf->gc_space_used -= iterator.leaf->getKVConsumedSpace(iterator.cur);
            ret = iterator.removeCurrent();
            ensure(ret == OP_RESULT::OK);
            iterator.mergeIfNeeded();
            COUNTERS_BLOCK() { WorkerCounters::myCounters().cc_todo_remove[btree.dt_id]++; }
         }
      }
   }
   jumpmuCatch() { UNREACHABLE(); }
}
// -------------------------------------------------------------------------------------
void BTreeVI::unlock(void* btree_object, const u8* entry_ptr)
{
   auto& btree = *reinterpret_cast<BTreeVI*>(btree_object);
   const auto& todo_entry = *reinterpret_cast<const UnlockEntry*>(entry_ptr);
   // -------------------------------------------------------------------------------------
   Slice key(todo_entry.key, todo_entry.key_length);
   OP_RESULT ret;
   // -------------------------------------------------------------------------------------
   jumpmuTry()
   {
      BTreeExclusiveIterator iterator(*static_cast<BTreeGeneric*>(&btree));
      ret = iterator.seekExact(key);
      ensure(ret == OP_RESULT::OK);
      // -------------------------------------------------------------------------------------
      MutableSlice primary_payload = iterator.mutableValue();
      Tuple& primary_version = *reinterpret_cast<Tuple*>(primary_payload.data());
      primary_version.read_lock_counter &= ~(1ull << cr::Worker::my().workerID());
   }
   jumpmuCatch() { UNREACHABLE(); }
}
// -------------------------------------------------------------------------------------
struct DTRegistry::DTMeta BTreeVI::getMeta()
{
   DTRegistry::DTMeta btree_meta = {.iterate_children = iterateChildrenSwips,
                                    .find_parent = findParent,
                                    .check_space_utilization = checkSpaceUtilization,
                                    .checkpoint = checkpoint,
                                    .undo = undo,
                                    .todo = todo,
                                    .unlock = unlock,
                                    .serialize = serialize,
                                    .deserialize = deserialize};
   return btree_meta;
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::scanDesc(u8* o_key, u16 o_key_length, function<bool(const u8*, u16, const u8*, u16)> callback, function<void()>)
{
   return scan<false>(o_key, o_key_length, callback);
}
// -------------------------------------------------------------------------------------
OP_RESULT BTreeVI::scanAsc(u8* o_key,
                           u16 o_key_length,
                           function<bool(const u8* key, u16 key_length, const u8* value, u16 value_length)> callback,
                           function<void()>)
{
   return scan<true>(o_key, o_key_length, callback);
}
// -------------------------------------------------------------------------------------
std::tuple<OP_RESULT, u16> BTreeVI::reconstructChainedTuple(BTreeSharedIterator& iterator, std::function<void(Slice value)> callback)
{
   u16 chain_length = 1;
   u16 materialized_value_length;
   std::unique_ptr<u8[]> materialized_value;
   Slice primary_payload = iterator.value();
   const ChainedTuple& tuple_head = *reinterpret_cast<const ChainedTuple*>(primary_payload.data());
   if (isVisibleForMe(tuple_head.worker_id, tuple_head.tx_id, false)) {
      if (tuple_head.is_removed) {
         return {OP_RESULT::NOT_FOUND, 1};
      } else {
         callback(Slice(tuple_head.payload, primary_payload.length() - sizeof(ChainedTuple)));
         return {OP_RESULT::OK, 1};
      }
   }
   // -------------------------------------------------------------------------------------
   // Head is not visible
   if (tuple_head.isFinal()) {
      return {OP_RESULT::NOT_FOUND, 1};
   }
   materialized_value_length = primary_payload.length() - sizeof(ChainedTuple);
   materialized_value = std::make_unique<u8[]>(materialized_value_length);
   std::memcpy(materialized_value.get(), tuple_head.payload, materialized_value_length);
   WORKERID next_worker_id = tuple_head.worker_id;
   TXID next_tx_id = tuple_head.tx_id;
   ChainSN next_command_id = tuple_head.command_id;
   // -------------------------------------------------------------------------------------
   while (true) {
      bool is_removed;
      bool found = cr::Worker::my().versions_space.retrieveVersion(next_tx_id, dt_id, next_command_id, [&](u8* version, u64 version_length) {
         const auto& secondary_version = *reinterpret_cast<const ChainedTupleVersion*>(version);
         if (secondary_version.is_delta) {
            // Apply delta
            const auto& update_descriptor = *reinterpret_cast<const UpdateSameSizeInPlaceDescriptor*>(secondary_version.payload);
            BTreeLL::applyDiff(update_descriptor, materialized_value.get(), secondary_version.payload + update_descriptor.size());
         } else {
            materialized_value_length = version_length - sizeof(ChainedTupleVersion);
            materialized_value = std::make_unique<u8[]>(materialized_value_length);
            std::memcpy(materialized_value.get(), secondary_version.payload, materialized_value_length);
         }
         // -------------------------------------------------------------------------------------
         is_removed = secondary_version.is_removed;
         next_worker_id = secondary_version.worker_id;
         next_tx_id = secondary_version.tx_id;
         next_command_id = secondary_version.command_id;
      });
      if (!found) {
         return {OP_RESULT::NOT_FOUND, chain_length};
      }
      if (isVisibleForMe(next_worker_id, next_tx_id, false)) {
         if (is_removed) {
            return {OP_RESULT::NOT_FOUND, chain_length};
         }
         callback(Slice(materialized_value.get(), materialized_value_length));
         return {OP_RESULT::OK, chain_length};
      }
      chain_length++;
      ensure(chain_length <= FLAGS_vi_max_chain_length);
   }
   return {OP_RESULT::NOT_FOUND, chain_length};
}
// -------------------------------------------------------------------------------------
}  // namespace btree
}  // namespace storage
}  // namespace leanstore
