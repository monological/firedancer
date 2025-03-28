#include <stdlib.h>
#define _GNU_SOURCE
#include "../../disco/tiles.h"
#include "generated/fd_exec_tile_seccomp.h"

#include "../../disco/topo/fd_pod_format.h"

#include "../../flamenco/runtime/fd_runtime.h"
#include "../../flamenco/runtime/fd_runtime_public.h"
#include "../../flamenco/runtime/fd_executor.h"
#include "../../flamenco/runtime/fd_hashes.h"

#include "../../funk/fd_funk.h"
#include "../../funk/fd_funk_filemap.h"

struct fd_exec_tile_ctx {

  /* link-related data structures. */
  ulong                 replay_exec_in_idx;
  ulong                 tile_cnt;
  ulong                 tile_idx;

  fd_wksp_t *           replay_in_mem;
  ulong                 replay_in_chunk0;
  ulong                 replay_in_wmark;

  /* Runtime public and local joins of its members. */
  fd_wksp_t *           runtime_public_wksp;
  fd_runtime_public_t * runtime_public;
  fd_spad_t const *     runtime_spad;

  /* Management around exec spad and frame lifetimes. We will always
     have 1 frame pushed onto the spad. This frame will contain the
     txn_ctx. Any allocations made for the scope of an epoch boundary
     will live in the next spad frame. Then, all allocations made for
     the scope of the slot ctx will live in the next spad frame.
     Finally, any state used for actually executing the transaction
     will use the remaining frames. The pending_{n}_pop variables are
     used to manage lifetimes we recieve slot/epoch updates. */
  fd_spad_t *           exec_spad;
  fd_wksp_t *           exec_spad_wksp;
  int                   pending_txn_pop;
  int                   pending_slot_pop;
  int                   pending_epoch_pop;

  /* Funk-specific setup.  */
  fd_funk_t *           funk;
  fd_wksp_t *           funk_wksp;

  /* Data structures related to managing and executing the transaction.
     The fd_txn_p_t is refreshed with every transaction and is sent
     from the dispatch/replay tile. The fd_exec_txn_ctx_t * is a valid
     local join that lives in the top-most frame of the spad that is
     setup when the exec tile is booted; its members are refreshed on
     the slot/epoch boundary. */
  fd_txn_p_t            txn;
  fd_exec_txn_ctx_t *   txn_ctx;
  int                   exec_res;
  uint                  flags;

  ulong *               exec_fseq;

};
typedef struct fd_exec_tile_ctx fd_exec_tile_ctx_t;

FD_FN_CONST static inline ulong
scratch_align( void ) {
  return 128UL;
}

FD_FN_PURE static inline ulong
scratch_footprint( fd_topo_tile_t const * tile FD_PARAM_UNUSED ) {
  /* clang-format off */
  ulong l = FD_LAYOUT_INIT;
  l       = FD_LAYOUT_APPEND( l, alignof(fd_exec_tile_ctx_t),  sizeof(fd_exec_tile_ctx_t) );
  return FD_LAYOUT_FINI( l, scratch_align() );
  /* clang-format on */
}

static void
prepare_new_epoch_execution( fd_exec_tile_ctx_t *            ctx,
                             fd_runtime_public_epoch_msg_t * epoch_msg ) {

  /* If we need to refresh epoch-level information, we need to pop off
     the transaction-level, slot-level, and epoch-level frames. */
  if( FD_LIKELY( ctx->pending_txn_pop ) ) {
    fd_spad_pop( ctx->exec_spad );
    ctx->pending_txn_pop = 0;
  }
  if( FD_LIKELY( ctx->pending_slot_pop ) ) {
    fd_spad_pop( ctx->exec_spad );
    ctx->pending_slot_pop = 0;
  }
  if( FD_LIKELY( ctx->pending_epoch_pop ) ) {
    fd_spad_pop( ctx->exec_spad );
    ctx->pending_epoch_pop = 0;
  }
  fd_spad_push( ctx->exec_spad );
  ctx->pending_epoch_pop = 1;

  ctx->txn_ctx->features          = epoch_msg->features;
  ctx->txn_ctx->total_epoch_stake = epoch_msg->total_epoch_stake;
  ctx->txn_ctx->schedule          = epoch_msg->epoch_schedule;
  ctx->txn_ctx->rent              = epoch_msg->rent;
  ctx->txn_ctx->slots_per_year    = epoch_msg->slots_per_year;

  uchar * stakes_enc = fd_wksp_laddr( ctx->runtime_public_wksp, epoch_msg->stakes_encoded_gaddr );
  if( FD_UNLIKELY( !stakes_enc ) ) {
    FD_LOG_ERR(( "Could not get laddr for encoded stakes" ));
  }

  fd_bincode_decode_ctx_t decode = {
    .data    = stakes_enc,
    .dataend = stakes_enc + epoch_msg->stakes_encoded_sz
  };
  ulong total_sz = 0UL;
  int   err      = fd_stakes_decode_footprint( &decode, &total_sz );
  if( FD_UNLIKELY( err ) ) {
    FD_LOG_ERR(( "Could not decode stakes footprint" ));
  }

  uchar *       stakes_mem = fd_spad_alloc( ctx->exec_spad, fd_stakes_align(), total_sz );
  fd_stakes_t * stakes     = fd_stakes_decode( stakes_mem, &decode );
  if( FD_UNLIKELY( !stakes ) ) {
    FD_LOG_ERR(( "Could not decode stakes" ));
  }
  ctx->txn_ctx->stakes = *stakes;
}

static void
prepare_new_slot_execution( fd_exec_tile_ctx_t *           ctx,
                            fd_runtime_public_slot_msg_t * slot_msg ) {

  /* If we need to refresh slot-level information, we need to pop off
     the transaction-level and slot-level frame. */
  if( FD_LIKELY( ctx->pending_txn_pop ) ) {
    fd_spad_pop( ctx->exec_spad );
    ctx->pending_txn_pop = 0;
  }
  if( FD_LIKELY( ctx->pending_slot_pop ) ) {
    fd_spad_pop( ctx->exec_spad );
    ctx->pending_slot_pop = 0;
  }
  fd_spad_push( ctx->exec_spad );
  ctx->pending_slot_pop = 1;

  fd_funk_txn_t * txn_map = fd_funk_txn_map( ctx->funk, ctx->funk_wksp );
  if( FD_UNLIKELY( !txn_map ) ) {
    FD_LOG_ERR(( "Could not find valid funk transaction map" ));
  }
  fd_funk_txn_xid_t xid = { .ul = { slot_msg->slot, slot_msg->slot } };
  fd_funk_txn_t * funk_txn = fd_funk_txn_query( &xid, txn_map );
  if( FD_UNLIKELY( !funk_txn ) ) {
    FD_LOG_ERR(( "Could not find valid funk transaction" ));
  }
  ctx->txn_ctx->funk_txn = funk_txn;

  ctx->txn_ctx->slot                        = slot_msg->slot;
  ctx->txn_ctx->prev_lamports_per_signature = slot_msg->prev_lamports_per_signature;
  ctx->txn_ctx->fee_rate_governor           = slot_msg->fee_rate_governor;

  ctx->txn_ctx->sysvar_cache = fd_wksp_laddr( ctx->runtime_public_wksp, slot_msg->sysvar_cache_gaddr );
  if( FD_UNLIKELY( !ctx->txn_ctx->sysvar_cache ) ) {
    FD_LOG_ERR(( "Could not find valid sysvar cache" ));
  }

  uchar * block_hash_queue_enc = fd_wksp_laddr( ctx->runtime_public_wksp, slot_msg->block_hash_queue_encoded_gaddr );
  fd_bincode_decode_ctx_t decode = {
    .data    = block_hash_queue_enc,
    .dataend = block_hash_queue_enc + slot_msg->block_hash_queue_encoded_sz
  };

  ulong total_sz = 0UL;
  int   err      = fd_block_hash_queue_decode_footprint( &decode, &total_sz );
  if( FD_UNLIKELY( err ) ) {
    FD_LOG_ERR(( "Could not decode block hash queue footprint" ));
  }

  uchar * block_hash_queue_mem = fd_spad_alloc( ctx->exec_spad, fd_block_hash_queue_align(), total_sz );
  fd_block_hash_queue_t * block_hash_queue = fd_block_hash_queue_decode( block_hash_queue_mem, &decode );
  if( FD_UNLIKELY( !block_hash_queue ) ) {
    FD_LOG_ERR(( "Could not decode block hash queue" ));
  }

  ctx->txn_ctx->block_hash_queue = *block_hash_queue;
}

static void
execute_txn( fd_exec_tile_ctx_t * ctx ) {
  if( FD_LIKELY( ctx->pending_txn_pop ) ) {
    fd_spad_pop( ctx->exec_spad );
    ctx->pending_txn_pop = 0;
  }
  fd_spad_push( ctx->exec_spad );
  ctx->pending_txn_pop = 1;

  fd_execute_txn_task_info_t task_info = {
    .txn_ctx  = ctx->txn_ctx,
    .exec_res = 0,
    .txn      = &ctx->txn,
  };

  fd_txn_t const * txn_descriptor = (fd_txn_t const *)task_info.txn->_;
  fd_rawtxn_b_t    raw_txn        = {
    .raw    = task_info.txn->payload,
    .txn_sz = (ushort)task_info.txn->payload_sz
  };

  fd_exec_txn_ctx_setup( ctx->txn_ctx, txn_descriptor, &raw_txn );

  int err = fd_executor_setup_accessed_accounts_for_txn( ctx->txn_ctx );
  if( FD_UNLIKELY( err ) ) {
    task_info.txn->flags = 0U;
    task_info.exec_res   = err;
    ctx->flags = 0U;
    return;
  }

  if( FD_UNLIKELY( fd_executor_txn_verify( ctx->txn_ctx )!=0 ) ) {
    FD_LOG_WARNING(( "sigverify failed: %s", FD_BASE58_ENC_64_ALLOCA( (uchar *)ctx->txn_ctx->_txn_raw->raw+ctx->txn_ctx->txn_descriptor->signature_off ) ));
    task_info.txn->flags = 0U;
    task_info.exec_res   = FD_RUNTIME_TXN_ERR_SIGNATURE_FAILURE;
    ctx->flags = 0U;
    return;
  }

  task_info.txn->flags |= FD_TXN_P_FLAGS_SANITIZE_SUCCESS;

  fd_runtime_pre_execute_check( &task_info, 0 );
  if( FD_UNLIKELY( !( task_info.txn->flags & FD_TXN_P_FLAGS_SANITIZE_SUCCESS ) ) ) {
    ctx->flags = 0U;
    return;
  }

  /* Execute */
  task_info.txn->flags |= FD_TXN_P_FLAGS_EXECUTE_SUCCESS;
  ctx->exec_res         = fd_execute_txn( &task_info );
  ctx->flags            = FD_TXN_P_FLAGS_EXECUTE_SUCCESS;

  if( FD_LIKELY( ctx->exec_res==FD_EXECUTOR_INSTR_SUCCESS ) ) {
    fd_txn_reclaim_accounts( task_info.txn_ctx );
  }
}

static void
hash_accounts( fd_exec_tile_ctx_t *                ctx,
               fd_runtime_public_hash_bank_msg_t * msg ) {

  ulong             start_idx = msg->start_idx;
  ulong             end_idx   = msg->end_idx;
  fd_lthash_value_t lt_hash;
  fd_lthash_zero( &lt_hash );

  fd_accounts_hash_task_info_t * task_info = fd_wksp_laddr( ctx->runtime_public_wksp, msg->task_infos_gaddr );
  if( FD_UNLIKELY( !task_info ) ) {
    FD_LOG_ERR(( "Unable to join task info array" ));
  }

  for( ulong i=start_idx; i<=end_idx; i++ ) {
    fd_account_hash( ctx->txn_ctx->acc_mgr,
                     ctx->txn_ctx->funk_txn,
                     &task_info[i],
                     &lt_hash,
                     ctx->txn_ctx->slot,
                     &ctx->txn_ctx->features );
  }
}

static void
during_frag( fd_exec_tile_ctx_t * ctx,
             ulong                in_idx,
             ulong                seq FD_PARAM_UNUSED,
             ulong                sig FD_PARAM_UNUSED,
             ulong                chunk,
             ulong                sz,
             ulong                ctl FD_PARAM_UNUSED ) {

  if( FD_UNLIKELY( in_idx == ctx->replay_exec_in_idx ) ) {
    if( FD_UNLIKELY( chunk < ctx->replay_in_chunk0 || chunk > ctx->replay_in_wmark ) ) {
      FD_LOG_ERR(( "chunk %lu %lu corrupt, not in range [%lu,%lu]",
                    chunk,
                    sz,
                    ctx->replay_in_chunk0,
                    ctx->replay_in_wmark ));
    }

    if( FD_LIKELY( sig==EXEC_NEW_TXN_SIG ) ) {
      fd_runtime_public_txn_msg_t * txn = (fd_runtime_public_txn_msg_t *)fd_chunk_to_laddr( ctx->replay_in_mem, chunk );
      fd_memcpy( &ctx->txn, &txn->txn, sizeof(fd_txn_p_t) );
      execute_txn( ctx );
      return;
    } else if( sig==EXEC_NEW_SLOT_SIG ) {
      fd_runtime_public_slot_msg_t * msg = fd_chunk_to_laddr( ctx->replay_in_mem, chunk );
      FD_LOG_NOTICE(( "new slot=%lu msg recvd", msg->slot ));
      prepare_new_slot_execution( ctx, msg );
      return;
    } else if( sig==EXEC_NEW_EPOCH_SIG ) {
      fd_runtime_public_epoch_msg_t * msg = fd_chunk_to_laddr( ctx->replay_in_mem, chunk );
      FD_LOG_NOTICE(( "new epoch=%lu msg recvd", msg->epoch_schedule.slots_per_epoch ));
      prepare_new_epoch_execution( ctx, msg );
      return;
    } else if( sig==EXEC_HASH_ACCS_SIG ) {
      fd_runtime_public_hash_bank_msg_t * msg = fd_chunk_to_laddr( ctx->replay_in_mem, chunk );
      FD_LOG_NOTICE(( "hash accs=%lu msg recvd", msg->end_idx - msg->start_idx ));
      hash_accounts( ctx, msg );
      return;
    } else {
      FD_LOG_ERR(( "Unknown signature" ));
    }

  }
}

static void
after_frag( fd_exec_tile_ctx_t * ctx    FD_PARAM_UNUSED,
            ulong                in_idx FD_PARAM_UNUSED,
            ulong                seq    FD_PARAM_UNUSED,
            ulong                sig,
            ulong                sz     FD_PARAM_UNUSED,
            ulong                tsorig FD_PARAM_UNUSED,
            ulong                tspub  FD_PARAM_UNUSED,
            fd_stem_context_t *  stem   FD_PARAM_UNUSED ) {

  if( sig==EXEC_NEW_SLOT_SIG ) {
    FD_LOG_DEBUG(( "Sending ack for new slot msg" ));
    fd_fseq_update( ctx->exec_fseq, fd_exec_fseq_set_slot_done() );
  } else if( sig==EXEC_NEW_EPOCH_SIG ) {
    FD_LOG_DEBUG(( "Sending ack for new epoch msg" ));
    fd_fseq_update( ctx->exec_fseq, fd_exec_fseq_set_epoch_done() );

  } else if( sig==EXEC_NEW_TXN_SIG ) {
    FD_LOG_DEBUG(( "Sending ack for new txn msg" ));
    /* At this point we can assume that the transaction is done
       executing. The replay tile will be repsonsible for commiting
       the transaction back to funk. */
    ctx->txn_ctx->exec_err = ctx->exec_res;
    ctx->txn_ctx->flags    = ctx->flags;
    fd_fseq_update( ctx->exec_fseq, fd_exec_fseq_set_txn_done() );
  } else if( sig==EXEC_HASH_ACCS_SIG ) {
    FD_LOG_NOTICE(( "Sending ack for hash accs msg" ));
  } else {
    FD_LOG_ERR(( "Unknown message signature" ));
  }
}

static void
privileged_init( fd_topo_t *      topo FD_PARAM_UNUSED,
                 fd_topo_tile_t * tile FD_PARAM_UNUSED ) {
}

static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile ) {

  /********************************************************************/
  /* validate allocations                                             */
  /********************************************************************/

  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_exec_tile_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_exec_tile_ctx_t), sizeof(fd_exec_tile_ctx_t) );
  ulong scratch_alloc_mem = FD_SCRATCH_ALLOC_FINI( l, scratch_align() );
  if( FD_UNLIKELY( scratch_alloc_mem - (ulong)scratch  - scratch_footprint( tile ) ) ) {
    FD_LOG_ERR( ( "Scratch_alloc_mem did not match scratch_footprint diff: %lu alloc: %lu footprint: %lu",
      scratch_alloc_mem - (ulong)scratch - scratch_footprint( tile ),
      scratch_alloc_mem,
      (ulong)scratch + scratch_footprint( tile ) ) );
  }

  /********************************************************************/
  /* validate links                                                   */
  /********************************************************************/

  ctx->tile_cnt = fd_topo_tile_name_cnt( topo, tile->name );
  ctx->tile_idx = tile->kind_id;


  /* First find and setup the in-link from replay to exec. */
  ctx->replay_exec_in_idx = fd_topo_find_tile_in_link( topo, tile, "replay_exec", ctx->tile_idx );
  if( FD_UNLIKELY( ctx->replay_exec_in_idx==ULONG_MAX ) ) {
    FD_LOG_ERR(( "Could not find replay_exec in-link" ));
  }
  fd_topo_link_t * replay_exec_in_link = &topo->links[tile->in_link_id[ctx->replay_exec_in_idx]];
  if( FD_UNLIKELY( !replay_exec_in_link) ) {
    FD_LOG_ERR(( "Invalid replay_exec in-link" ));
  }
  ctx->replay_in_mem    = topo->workspaces[topo->objs[replay_exec_in_link->dcache_obj_id].wksp_id].wksp;
  ctx->replay_in_chunk0 = fd_dcache_compact_chunk0( ctx->replay_in_mem, replay_exec_in_link->dcache );
  ctx->replay_in_wmark  = fd_dcache_compact_wmark( ctx->replay_in_mem,
                                                   replay_exec_in_link->dcache,
                                                   replay_exec_in_link->mtu );

  /********************************************************************/
  /* runtime public                                                   */
  /********************************************************************/

  ulong runtime_obj_id = fd_pod_queryf_ulong( topo->props, ULONG_MAX, "runtime_pub" );
  if( FD_UNLIKELY( runtime_obj_id==ULONG_MAX ) ) {
    FD_LOG_ERR(( "Could not find topology object for runtime public" ));
  }

  ctx->runtime_public_wksp = topo->workspaces[ topo->objs[ runtime_obj_id ].wksp_id ].wksp;

  if( FD_UNLIKELY( ctx->runtime_public_wksp==NULL ) ) {
    FD_LOG_ERR(( "No runtime_public workspace" ));
  }

  ctx->runtime_public = fd_runtime_public_join( fd_topo_obj_laddr( topo, runtime_obj_id ) );
  if( FD_UNLIKELY( !ctx->runtime_public ) ) {
    FD_LOG_ERR(( "Failed to join runtime public" ));
  }

  ctx->runtime_spad = fd_runtime_public_join_and_get_runtime_spad( ctx->runtime_public );
  if( FD_UNLIKELY( !ctx->runtime_spad ) ) {
    FD_LOG_ERR(( "Failed to get and join runtime spad" ));
  }

  /********************************************************************/
  /* spad allocator                                                   */
  /********************************************************************/

  /* First join the correct exec spad and hten the correct runtime spad
     which lives inside of the runtime public wksp. */

  ulong exec_spad_obj_id = fd_pod_queryf_ulong( topo->props, ULONG_MAX, "exec_spad.%lu", ctx->tile_idx );
  if( FD_UNLIKELY( exec_spad_obj_id==ULONG_MAX ) ) {
    FD_LOG_ERR(( "Could not find topology object for exec spad" ));
  }

  ctx->exec_spad = fd_spad_join( fd_topo_obj_laddr( topo, exec_spad_obj_id ) );
  if( FD_UNLIKELY( !ctx->exec_spad ) ) {
    FD_LOG_ERR(( "Failed to join exec spad" ));
  }
  ctx->exec_spad_wksp = fd_wksp_containing( ctx->exec_spad );

  ctx->pending_txn_pop   = 0;
  ctx->pending_slot_pop  = 0;
  ctx->pending_epoch_pop = 0;

  /********************************************************************/
  /* funk-specific setup                                              */
  /********************************************************************/

  /* Setting these parameters are not required because we are joining
     the funk that was setup in the replay tile. */
  FD_LOG_NOTICE(( "Trying to join funk at file=%s", tile->exec.funk_file ));
  ctx->funk = fd_funk_open_file( tile->exec.funk_file,
                                  1UL,
                                  0UL,
                                  0UL,
                                  0UL,
                                  0UL,
                                  FD_FUNK_READONLY,
                                  NULL );
  ctx->funk_wksp = fd_funk_wksp( ctx->funk );
  if( FD_UNLIKELY( !ctx->funk ) ) {
    FD_LOG_ERR(( "failed to join a funk" ));
  }

  FD_LOG_NOTICE(( "Just joined funk at file=%s", tile->exec.funk_file ));

  /********************************************************************/
  /* setup txn ctx                                                    */
  /********************************************************************/

  fd_spad_push( ctx->exec_spad );
  ctx->pending_txn_pop  = 0;
  ctx->pending_slot_pop = 0;
  uchar * txn_ctx_mem   = fd_spad_alloc( ctx->exec_spad, FD_EXEC_TXN_CTX_ALIGN, FD_EXEC_TXN_CTX_FOOTPRINT );
  ctx->txn_ctx          = fd_exec_txn_ctx_join( fd_exec_txn_ctx_new( txn_ctx_mem ), ctx->exec_spad, ctx->exec_spad_wksp );

  uchar *        acc_mgr_mem     = fd_spad_alloc( ctx->exec_spad, FD_ACC_MGR_ALIGN, FD_ACC_MGR_FOOTPRINT );
  fd_acc_mgr_t * acc_mgr         = fd_acc_mgr_new( acc_mgr_mem, ctx->funk );
  ctx->txn_ctx->acc_mgr          = acc_mgr;
  if( FD_UNLIKELY( !ctx->txn_ctx->acc_mgr ) ) {
    FD_LOG_ERR(( "Failed to create account manager" ));
  }

  ctx->txn_ctx->runtime_pub_wksp = ctx->runtime_public_wksp;
  if( FD_UNLIKELY( !ctx->txn_ctx->runtime_pub_wksp ) ) {
    FD_LOG_ERR(( "Failed to find public wksp" ));
  }

  /********************************************************************/
  /* setup exec fseq                                                  */
  /********************************************************************/

  ulong exec_fseq_id = fd_pod_queryf_ulong( topo->props, ULONG_MAX, "exec_fseq.%lu", ctx->tile_idx );
  ctx->exec_fseq = fd_fseq_join( fd_topo_obj_laddr( topo, exec_fseq_id ) );
  if( FD_UNLIKELY( !ctx->exec_fseq ) ) {
    FD_LOG_ERR(( "exec tile %lu has no fseq", ctx->tile_idx ));
  }
  fd_fseq_update( ctx->exec_fseq, FD_EXEC_STATE_NOT_BOOTED );


  ulong txn_ctx_gaddr = fd_wksp_gaddr( ctx->exec_spad_wksp, ctx->txn_ctx );
  if( FD_UNLIKELY( !txn_ctx_gaddr ) ) {
    FD_LOG_ERR(( "Could not get gaddr for txn_ctx" ));
  }

  ulong exec_spad_gaddr = fd_wksp_gaddr( ctx->exec_spad_wksp, ctx->exec_spad );
  if( FD_UNLIKELY( !exec_spad_gaddr ) ) {
    FD_LOG_ERR(( "Could not get gaddr for exec_spad" ));
  }

  if( FD_UNLIKELY( txn_ctx_gaddr-exec_spad_gaddr>UINT_MAX ) ) {
    FD_LOG_ERR(( "Txn ctx offset from exec spad is too large" ));
  }

  uint txn_ctx_offset = (uint)(txn_ctx_gaddr-exec_spad_gaddr);
  fd_fseq_update( ctx->exec_fseq, fd_exec_fseq_set_booted( txn_ctx_offset ) );

  FD_LOG_NOTICE(( "Done booting exec tile idx=%lu", ctx->tile_idx ));
}

static void
after_credit( fd_exec_tile_ctx_t * ctx,
              fd_stem_context_t *  stem        FD_PARAM_UNUSED,
              int *                opt_poll_in FD_PARAM_UNUSED,
              int *                charge_busy FD_PARAM_UNUSED ) {
  (void)ctx;
}

static ulong
populate_allowed_seccomp( fd_topo_t const *      topo,
                          fd_topo_tile_t const * tile,
                          ulong                  out_cnt,
                          struct sock_filter *   out ) {
  (void)topo;
  (void)tile;

  populate_sock_filter_policy_fd_exec_tile( out_cnt, out, (uint)fd_log_private_logfile_fd() );
  return sock_filter_policy_fd_exec_tile_instr_cnt;
}

static ulong
populate_allowed_fds( fd_topo_t const *      topo,
                      fd_topo_tile_t const * tile,
                      ulong                  out_fds_cnt,
                      int *                  out_fds ) {
  (void)topo;
  (void)tile;

  if( FD_UNLIKELY( out_fds_cnt<2UL ) ) FD_LOG_ERR(( "out_fds_cnt %lu", out_fds_cnt ));

  ulong out_cnt = 0UL;
  out_fds[ out_cnt++ ] = 2; /* stderr */
  if( FD_LIKELY( -1!=fd_log_private_logfile_fd() ) )
    out_fds[ out_cnt++ ] = fd_log_private_logfile_fd(); /* logfile */
  return out_cnt;
}


#define STEM_BURST (1UL)

#define STEM_CALLBACK_CONTEXT_TYPE  fd_exec_tile_ctx_t
#define STEM_CALLBACK_CONTEXT_ALIGN alignof(fd_exec_tile_ctx_t)

#define STEM_CALLBACK_DURING_FRAG  during_frag
#define STEM_CALLBACK_AFTER_FRAG   after_frag
#define STEM_CALLBACK_AFTER_CREDIT after_credit


#include "../../disco/stem/fd_stem.c"

fd_topo_run_tile_t fd_tile_execor = {
    .name                     = "exec",
    .loose_footprint          = 0UL,
    .populate_allowed_seccomp = populate_allowed_seccomp,
    .populate_allowed_fds     = populate_allowed_fds,
    .scratch_align            = scratch_align,
    .scratch_footprint        = scratch_footprint,
    .privileged_init          = privileged_init,
    .unprivileged_init        = unprivileged_init,
    .run                      = stem_run,
};
