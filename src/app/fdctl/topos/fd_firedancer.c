/* Firedancer topology used for testing the full validator.
   Associated test script: test_firedancer.sh */
#include "../fdctl.h"

#include "../../../discof/geyser/fd_replay_notif.h"
#include "../../../disco/net/fd_net_tile.h"
#include "../../../disco/quic/fd_tpu.h"
#include "../../../disco/tiles.h"
#include "../../../disco/topo/fd_topob.h"
#include "../../../disco/topo/fd_cpu_topo.h"
#include "../../../disco/topo/fd_pod_format.h"
#include "../../../flamenco/runtime/fd_blockstore.h"
#include "../../../flamenco/runtime/fd_runtime.h"
#include "../../../flamenco/runtime/fd_runtime_public.h"
#include "../../../flamenco/runtime/fd_txncache.h"
#include "../../../util/tile/fd_tile_private.h"

#include <sys/random.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

static fd_topo_obj_t *
setup_topo_blockstore( fd_topo_t *  topo,
                       char const * wksp_name,
                       ulong        shred_max,
                       ulong        block_max,
                       ulong        idx_max,
                       ulong        txn_max,
                       ulong        alloc_max ) {
  fd_topo_obj_t * obj = fd_topob_obj( topo, "blockstore", wksp_name );

  ulong seed;
  FD_TEST( sizeof(ulong) == getrandom( &seed, sizeof(ulong), 0 ) );

  FD_TEST( fd_pod_insertf_ulong( topo->props, 1UL,        "obj.%lu.wksp_tag",   obj->id ) );
  FD_TEST( fd_pod_insertf_ulong( topo->props, seed,       "obj.%lu.seed",       obj->id ) );
  FD_TEST( fd_pod_insertf_ulong( topo->props, shred_max,  "obj.%lu.shred_max",  obj->id ) );
  FD_TEST( fd_pod_insertf_ulong( topo->props, block_max,  "obj.%lu.block_max",  obj->id ) );
  FD_TEST( fd_pod_insertf_ulong( topo->props, idx_max,    "obj.%lu.idx_max",    obj->id ) );
  FD_TEST( fd_pod_insertf_ulong( topo->props, txn_max,    "obj.%lu.txn_max",    obj->id ) );
  FD_TEST( fd_pod_insertf_ulong( topo->props, alloc_max,  "obj.%lu.alloc_max",  obj->id ) );

  /* DO NOT MODIFY LOOSE WITHOUT CHANGING HOW BLOCKSTORE ALLOCATES INTERNAL STRUCTURES */

  ulong blockstore_footprint = fd_blockstore_footprint( shred_max, block_max, idx_max, txn_max ) + alloc_max;
  FD_TEST( fd_pod_insertf_ulong( topo->props, blockstore_footprint,  "obj.%lu.loose", obj->id ) );

  return obj;
}

fd_topo_obj_t *
setup_topo_runtime_pub( fd_topo_t *  topo, char const * wksp_name ) {
  fd_topo_obj_t * obj = fd_topob_obj( topo, "runtime_pub", wksp_name );

  FD_TEST( fd_pod_insertf_ulong( topo->props, 12UL,        "obj.%lu.wksp_tag",   obj->id ) );

  ulong footprint = fd_runtime_public_footprint();
  FD_TEST( fd_pod_insertf_ulong( topo->props, footprint,  "obj.%lu.loose", obj->id ) );

  return obj;
}

static fd_topo_obj_t *
setup_topo_txncache( fd_topo_t *  topo,
                     char const * wksp_name,
                     ulong        max_rooted_slots,
                     ulong        max_live_slots,
                     ulong        max_txn_per_slot ) {
  fd_topo_obj_t * obj = fd_topob_obj( topo, "txncache", wksp_name );

  FD_TEST( fd_pod_insertf_ulong( topo->props, max_rooted_slots, "obj.%lu.max_rooted_slots", obj->id ) );
  FD_TEST( fd_pod_insertf_ulong( topo->props, max_live_slots,   "obj.%lu.max_live_slots",   obj->id ) );
  FD_TEST( fd_pod_insertf_ulong( topo->props, max_txn_per_slot, "obj.%lu.max_txn_per_slot", obj->id ) );

  return obj;
}

static int
resolve_gossip_entrypoint( char const *    host_port,
                           fd_ip4_port_t * ip4_port ) {

  /* Split host:port */

  char const * colon = strrchr( host_port, ':' );
  if( FD_UNLIKELY( !colon ) ) {
    FD_LOG_ERR(( "invalid [gossip.entrypoints] entry \"%s\": no port number", host_port ));
  }

  char fqdn[ 255 ];
  ulong fqdn_len = (ulong)( colon-host_port );
  if( FD_UNLIKELY( fqdn_len>254 ) ) {
    FD_LOG_ERR(( "invalid [gossip.entrypoints] entry \"%s\": hostname too long", host_port ));
  }
  fd_memcpy( fqdn, host_port, fqdn_len );
  fqdn[ fqdn_len ] = '\0';

  /* Parse port number */

  char const * port_str = colon+1;
  char const * endptr   = NULL;
  ulong port = strtoul( port_str, (char **)&endptr, 10 );
  if( FD_UNLIKELY( !endptr || !port || port>USHORT_MAX || *endptr!='\0' ) ) {
    FD_LOG_ERR(( "invalid [gossip.entrypoints] entry \"%s\": invalid port number", host_port ));
  }
  ip4_port->port = (ushort)fd_ushort_bswap( (ushort)port );

  /* Resolve hostname */

  struct addrinfo hints = { .ai_family = AF_INET };
  struct addrinfo * res;
  int err = getaddrinfo( fqdn, NULL, &hints, &res );
  if( FD_UNLIKELY( err ) ) {
    FD_LOG_WARNING(( "cannot resolve [gossip.entrypoints] entry \"%s\": %i-%s", fqdn, err, gai_strerror( err ) ));
    return 0;
  }

  int resolved = 0;
  for( struct addrinfo * cur=res; cur; cur=cur->ai_next ) {
    if( FD_UNLIKELY( cur->ai_addr->sa_family!=AF_INET ) ) continue;
    struct sockaddr_in const * addr = (struct sockaddr_in const *)cur->ai_addr;
    ip4_port->addr = addr->sin_addr.s_addr;
    resolved = 1;
    break;
  }

  freeaddrinfo( res );
  return resolved;
}

static void
resolve_gossip_entrypoints( config_t * config ) {
  ulong entrypoint_cnt = config->gossip.entrypoints_cnt;
  ulong resolved_entrypoints = 0UL;
  for( ulong j=0UL; j<entrypoint_cnt; j++ ) {
    if( resolve_gossip_entrypoint( config->gossip.entrypoints[j], &config->gossip.resolved_entrypoints[resolved_entrypoints] ) ) {
      resolved_entrypoints++;
    }
  }
  config->gossip.resolved_entrypoints_cnt = resolved_entrypoints;
}

void
fd_topo_initialize( config_t * config ) {
  resolve_gossip_entrypoints( config );

  ulong net_tile_cnt    = config->layout.net_tile_count;
  ulong shred_tile_cnt  = config->layout.shred_tile_count;
  ulong quic_tile_cnt   = config->layout.quic_tile_count;
  ulong verify_tile_cnt = config->layout.verify_tile_count;
  ulong bank_tile_cnt   = config->layout.bank_tile_count;
  ulong exec_tile_cnt   = config->layout.exec_tile_count;

  ulong replay_tpool_thread_count = config->tiles.replay.tpool_thread_count;
  ulong batch_tpool_thread_count  = config->tiles.batch.hash_tpool_thread_count;

  int enable_rpc = ( config->rpc.port != 0 );

  fd_topo_t * topo = { fd_topob_new( &config->topo, config->name ) };
  topo->max_page_size = fd_cstr_to_shmem_page_sz( config->hugetlbfs.max_page_size );
  topo->gigantic_page_threshold = config->hugetlbfs.gigantic_page_threshold_mib << 20;

  /*             topo, name */
  fd_topob_wksp( topo, "metric_in"  );
  fd_topob_wksp( topo, "net_shred"  );
  fd_topob_wksp( topo, "net_gossip" );
  fd_topob_wksp( topo, "net_repair" );
  fd_topob_wksp( topo, "net_quic"   );
  fd_topob_wksp( topo, "net_voter"  );

  fd_topob_wksp( topo, "quic_verify"  );
  fd_topob_wksp( topo, "verify_dedup" );
  fd_topob_wksp( topo, "dedup_pack"   );

  fd_topob_wksp( topo, "shred_storei" );
  fd_topob_wksp( topo, "shred_replay" );
  fd_topob_wksp( topo, "stake_out"    );

  fd_topob_wksp( topo, "poh_shred"    );

  fd_topob_wksp( topo, "shred_sign"   );
  fd_topob_wksp( topo, "sign_shred"   );

  fd_topob_wksp( topo, "gossip_sign"  );
  fd_topob_wksp( topo, "sign_gossip"  );

  fd_topob_wksp( topo, "replay_exec"  );

  fd_topob_wksp( topo, "voter_sign"   );
  fd_topob_wksp( topo, "sign_voter"   );

  fd_topob_wksp( topo, "crds_shred"   );
  fd_topob_wksp( topo, "gossip_repai" );
  fd_topob_wksp( topo, "gossip_verif" );
  fd_topob_wksp( topo, "gossip_eqvoc" );

  fd_topob_wksp( topo, "store_repair" );
  fd_topob_wksp( topo, "repair_store" );

  fd_topob_wksp( topo, "repair_sign"  );
  fd_topob_wksp( topo, "sign_repair"  );

  fd_topob_wksp( topo, "store_replay" );
  fd_topob_wksp( topo, "repair_repla" );
  fd_topob_wksp( topo, "replay_poh"   );
  fd_topob_wksp( topo, "replay_notif" );
  fd_topob_wksp( topo, "bank_busy"    );
  fd_topob_wksp( topo, "root_slot"    );
  fd_topob_wksp( topo, "pack_replay"  );
  fd_topob_wksp( topo, "replay_voter" );
  fd_topob_wksp( topo, "gossip_voter" );
  fd_topob_wksp( topo, "voter_gossip" );
  fd_topob_wksp( topo, "voter_dedup"  );
  fd_topob_wksp( topo, "batch_replay" );

  fd_topob_wksp( topo, "rstart_gossi" );
  fd_topob_wksp( topo, "gossi_rstart" );
  fd_topob_wksp( topo, "rstart_store" );
  fd_topob_wksp( topo, "store_rstart" );

  fd_topob_wksp( topo, "quic"        );
  fd_topob_wksp( topo, "verify"      );
  fd_topob_wksp( topo, "dedup"       );
  fd_topob_wksp( topo, "shred"       );
  fd_topob_wksp( topo, "pack"        );
  fd_topob_wksp( topo, "storei"      );
  fd_topob_wksp( topo, "sign"        );
  fd_topob_wksp( topo, "repair"      );
  fd_topob_wksp( topo, "gossip"      );
  fd_topob_wksp( topo, "metric"      );
  fd_topob_wksp( topo, "replay"      );
  fd_topob_wksp( topo, "runtime_pub" );
  fd_topob_wksp( topo, "exec"        );
  fd_topob_wksp( topo, "rtpool"      );
  fd_topob_wksp( topo, "bhole"       );
  fd_topob_wksp( topo, "bstore"      );
  fd_topob_wksp( topo, "tcache"      );
  fd_topob_wksp( topo, "pohi"        );
  fd_topob_wksp( topo, "voter"       );
  fd_topob_wksp( topo, "poh_slot"    );
  fd_topob_wksp( topo, "eqvoc"       );
  fd_topob_wksp( topo, "batch"       );
  fd_topob_wksp( topo, "btpool"      );
  fd_topob_wksp( topo, "constipate"  );
  fd_topob_wksp( topo, "restart"     );
  fd_topob_wksp( topo, "exec_spad"   );
  fd_topob_wksp( topo, "exec_fseq"   );

  if( enable_rpc ) fd_topob_wksp( topo, "rpcsrv" );

  #define FOR(cnt) for( ulong i=0UL; i<cnt; i++ )

  /*                                  topo, link_name,      wksp_name,      depth,                                    mtu,                           burst */
  FOR(quic_tile_cnt)   fd_topob_link( topo, "quic_net",     "net_quic",     config->tiles.net.send_buffer_size,       FD_NET_MTU,                    1UL );
  FOR(shred_tile_cnt)  fd_topob_link( topo, "shred_net",    "net_shred",    config->tiles.net.send_buffer_size,       FD_NET_MTU,                    1UL );
  FOR(quic_tile_cnt)   fd_topob_link( topo, "quic_verify",  "quic_verify",  config->tiles.verify.receive_buffer_size, FD_TPU_REASM_MTU,              config->tiles.quic.txn_reassembly_count );
  FOR(verify_tile_cnt) fd_topob_link( topo, "verify_dedup", "verify_dedup", config->tiles.verify.receive_buffer_size, FD_TPU_PARSED_MTU,             1UL );
  /**/                 fd_topob_link( topo, "dedup_pack",   "dedup_pack",   config->tiles.verify.receive_buffer_size, FD_TPU_PARSED_MTU,             1UL );

  /**/                 fd_topob_link( topo, "stake_out",    "stake_out",    128UL,                                    40UL + 40200UL * 40UL,         1UL );
  /* See long comment in fd_shred.c for an explanation about the size of this dcache. */
  FOR(shred_tile_cnt)  fd_topob_link( topo, "shred_storei", "shred_storei", 65536UL,                                  4UL*FD_SHRED_STORE_MTU,        4UL+config->tiles.shred.max_pending_shred_sets );

  FOR(shred_tile_cnt)  fd_topob_link( topo, "shred_sign",   "shred_sign",   128UL,                                    32UL,                          1UL );
  FOR(shred_tile_cnt)  fd_topob_link( topo, "sign_shred",   "sign_shred",   128UL,                                    64UL,                          1UL );
  FOR(shred_tile_cnt)  fd_topob_link( topo, "shred_replay", "shred_replay", 128UL,                                    128UL,                         config->tiles.shred.max_pending_shred_sets );

  /**/                 fd_topob_link( topo, "gossip_sign",  "gossip_sign",  128UL,                                    2048UL,                        1UL );
  /**/                 fd_topob_link( topo, "sign_gossip",  "sign_gossip",  128UL,                                    64UL,                          1UL );
  FOR(exec_tile_cnt)   fd_topob_link( topo, "replay_exec",  "replay_exec",   128UL,                                   10240UL,                       1UL );

  /**/                 fd_topob_link( topo, "gossip_verif", "gossip_verif", config->tiles.verify.receive_buffer_size, FD_TPU_MTU,                    1UL );
  /**/                 fd_topob_link( topo, "gossip_eqvoc", "gossip_eqvoc", 128UL,                                    FD_TPU_MTU,                    1UL );

  /**/                 fd_topob_link( topo, "crds_shred",   "crds_shred",   128UL,                                    8UL  + 40200UL * 38UL,         1UL );
  /**/                 fd_topob_link( topo, "gossip_repai", "gossip_repai", 128UL,                                    40200UL * 38UL, 1UL );
  /**/                 fd_topob_link( topo, "gossip_voter", "gossip_voter", 128UL,                                    40200UL * 38UL, 1UL );

  /**/                 fd_topob_link( topo, "gossip_net",   "net_gossip",   config->tiles.net.send_buffer_size,       FD_NET_MTU,                    1UL );
  /**/                 fd_topob_link( topo, "voter_net",    "net_voter",    config->tiles.net.send_buffer_size,       FD_NET_MTU,                    1UL );
  /**/                 fd_topob_link( topo, "voter_dedup",  "voter_dedup",  128UL,                                    FD_TPU_MTU,                    1UL );

  /**/                 fd_topob_link( topo, "store_repair", "store_repair", 1024UL,                                   USHORT_MAX,                    16UL  );
  /**/                 fd_topob_link( topo, "repair_store", "repair_store", 1024UL*1024UL,                            FD_SHRED_MAX_SZ,               128UL );
  /**/                 fd_topob_link( topo, "repair_net",   "net_repair",   config->tiles.net.send_buffer_size,       FD_NET_MTU,                    1UL   );
  /**/                 fd_topob_link( topo, "repair_sign",  "repair_sign",  128UL,                                    2048UL,                        1UL );
  /**/                 fd_topob_link( topo, "sign_repair",  "sign_repair",  128UL,                                    64UL,                          1UL );
  /**/                 fd_topob_link( topo, "repair_repla", "repair_repla", 2048UL,                                   sizeof(ulong),                 1UL   );
  /**/                 fd_topob_link( topo, "store_replay", "store_replay", 32768UL,                                  sizeof(ulong),                 64UL  );
  FOR(bank_tile_cnt)   fd_topob_link( topo, "replay_poh",   "replay_poh",   128UL,                                    (4096UL*sizeof(fd_txn_p_t))+sizeof(fd_microblock_trailer_t), 1UL  );
  /**/                 fd_topob_link( topo, "replay_notif", "replay_notif", FD_REPLAY_NOTIF_DEPTH,                    FD_REPLAY_NOTIF_MTU,           1UL   );
  /**/                 fd_topob_link( topo, "poh_shred",    "poh_shred",    16384UL,                                  USHORT_MAX,                    1UL   );
  /**/                 fd_topob_link( topo, "pack_replay",  "pack_replay",  65536UL,                                  USHORT_MAX,                    1UL   );
  /**/                 fd_topob_link( topo, "poh_pack",     "replay_poh",   128UL,                                    sizeof(fd_became_leader_t) ,   1UL   );

  /**/                 fd_topob_link( topo, "replay_voter", "replay_voter", 128UL,                                    sizeof(fd_txn_p_t),            1UL   );
  /**/                 fd_topob_link( topo, "voter_gossip", "voter_gossip", 128UL,                                    FD_TXN_MTU,                    1UL   );
  /**/                 fd_topob_link( topo, "voter_sign",   "voter_sign",   128UL,                                    FD_TXN_MTU,                    1UL   );
  /**/                 fd_topob_link( topo, "sign_voter",   "sign_voter",   128UL,                                    64UL,                          1UL   );

  /**/                 fd_topob_link( topo, "batch_replay", "batch_replay", 128UL,                                    32UL,                          1UL   );
  /**/                 fd_topob_link( topo, "rstart_gossi", "rstart_gossi", 128UL,                                    4UL + 128UL + 8192UL,          1UL );
  /**/                 fd_topob_link( topo, "gossi_rstart", "gossi_rstart", 128UL,                                    4UL + 128UL + 8192UL,          1UL );
  /**/                 fd_topob_link( topo, "rstart_store", "rstart_store", 128UL,                                    sizeof(ulong) * 2,             1UL );
  /**/                 fd_topob_link( topo, "store_rstart", "store_rstart", 128UL,                                    sizeof(fd_funk_txn_xid_t),     1UL );

  ushort parsed_tile_to_cpu[ FD_TILE_MAX ];
  /* Unassigned tiles will be floating, unless auto topology is enabled. */
  for( ulong i=0UL; i<FD_TILE_MAX; i++ ) parsed_tile_to_cpu[ i ] = USHORT_MAX;

  int is_auto_affinity = !strcmp( config->layout.affinity, "auto" );
  int is_bench_auto_affinity = !strcmp( config->development.bench.affinity, "auto" );

  if( FD_UNLIKELY( is_auto_affinity != is_bench_auto_affinity ) ) {
    FD_LOG_ERR(( "The CPU affinity string in the configuration file under [layout.affinity] and [development.bench.affinity] must all be set to 'auto' or all be set to a specific CPU affinity string." ));
  }

  fd_topo_cpus_t cpus[1];
  fd_topo_cpus_init( cpus );

  ulong affinity_tile_cnt = 0UL;
  if( FD_LIKELY( !is_auto_affinity ) ) affinity_tile_cnt = fd_tile_private_cpus_parse( config->layout.affinity, parsed_tile_to_cpu );

  ulong tile_to_cpu[ FD_TILE_MAX ];
  for( ulong i=0UL; i<affinity_tile_cnt; i++ ) {
    if( FD_UNLIKELY( parsed_tile_to_cpu[ i ]!=USHORT_MAX && parsed_tile_to_cpu[ i ]>=cpus->cpu_cnt ) )
      FD_LOG_ERR(( "The CPU affinity string in the configuration file under [layout.affinity] specifies a CPU index of %hu, but the system "
                   "only has %lu CPUs. You should either change the CPU allocations in the affinity string, or increase the number of CPUs "
                   "in the system.",
                   parsed_tile_to_cpu[ i ], cpus->cpu_cnt ));
    tile_to_cpu[ i ] = fd_ulong_if( parsed_tile_to_cpu[ i ]==USHORT_MAX, ULONG_MAX, (ulong)parsed_tile_to_cpu[ i ] );
  }

  fd_topos_net_tiles( topo, config->layout.net_tile_count, config->tiles.netlink.max_routes, config->tiles.netlink.max_neighbors, config->development.net.provider, config->tiles.net.interface, config->tiles.net.bind_address_parsed, config->tiles.net.flush_timeout_micros,
    config->tiles.net.xdp_rx_queue_size, config->tiles.net.xdp_tx_queue_size, config->tiles.net.xdp_zero_copy, config->tiles.net.xdp_mode, config->development.net.sock_receive_buffer_size, config->development.net.sock_send_buffer_size, tile_to_cpu );

  FOR(net_tile_cnt) fd_topos_net_rx_link( topo, "net_gossip", i, config->tiles.net.send_buffer_size );
  FOR(net_tile_cnt) fd_topos_net_rx_link( topo, "net_repair", i, config->tiles.net.send_buffer_size );
  FOR(net_tile_cnt) fd_topos_net_rx_link( topo, "net_quic",   i, config->tiles.net.send_buffer_size );
  FOR(net_tile_cnt) fd_topos_net_rx_link( topo, "net_shred",  i, config->tiles.net.send_buffer_size );

  /*                                              topo, tile_name, tile_wksp, metrics_wksp, cpu_idx,                       is_agave, uses_keyswitch */
  FOR(quic_tile_cnt)               fd_topob_tile( topo, "quic",    "quic",    "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  FOR(verify_tile_cnt)             fd_topob_tile( topo, "verify",  "verify",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "dedup",   "dedup",   "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  FOR(shred_tile_cnt)              fd_topob_tile( topo, "shred",   "shred",   "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        1 );
  /**/                             fd_topob_tile( topo, "storei",  "storei",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "sign",    "sign",    "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        1 );
  /**/                             fd_topob_tile( topo, "metric",  "metric",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "pack",    "pack",    "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "pohi",    "pohi",    "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "gossip",  "gossip",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "repair",  "repair",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "sender",  "voter",   "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "bhole",   "bhole",   "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "eqvoc",   "eqvoc",   "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );

  /**/                             fd_topob_tile( topo, "replay",  "replay",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /* These thread tiles must be defined immediately after the replay tile.  We subtract one because the replay tile acts as a thread in the tpool as well. */
  FOR(replay_tpool_thread_count-1) fd_topob_tile( topo, "rtpool", "rtpool", "metric_in", tile_to_cpu[ topo->tile_cnt ],    0,        0 );
  FOR(exec_tile_cnt)               fd_topob_tile( topo, "exec",    "exec",    "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /**/                             fd_topob_tile( topo, "batch",   "batch",   "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  /* These thread tiles must be defined immediately after the snapshot tile. */
  FOR(batch_tpool_thread_count-1)  fd_topob_tile( topo, "btpool",  "btpool",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );

  /* TODO: not launching the restart tile if in_wen_restart is false */
  //if( FD_UNLIKELY( config->tiles.restart.in_wen_restart ) ) {
  /**/                             fd_topob_tile( topo, "rstart",  "restart", "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
  //}

  if( enable_rpc )                 fd_topob_tile( topo, "rpcsrv",  "rpcsrv",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );

  fd_topo_tile_t * store_tile  = &topo->tiles[ fd_topo_find_tile( topo, "storei", 0UL ) ];
  fd_topo_tile_t * replay_tile = &topo->tiles[ fd_topo_find_tile( topo, "replay", 0UL ) ];
  fd_topo_tile_t * repair_tile = &topo->tiles[ fd_topo_find_tile( topo, "repair", 0UL ) ];
  fd_topo_tile_t * batch_tile  = &topo->tiles[ fd_topo_find_tile( topo, "batch" , 0UL ) ];
  fd_topo_tile_t * pack_tile   = &topo->tiles[ fd_topo_find_tile( topo, "pack"  , 0UL ) ];
  fd_topo_tile_t * exec_tile   = &topo->tiles[ fd_topo_find_tile( topo, "exec"  , 0UL ) ];

  /* Create a shared blockstore to be used by store and replay. */
  fd_topo_obj_t * blockstore_obj = setup_topo_blockstore( topo,
                                                          "bstore",
                                                          config->blockstore.shred_max,
                                                          config->blockstore.block_max,
                                                          config->blockstore.idx_max,
                                                          config->blockstore.txn_max,
                                                          config->blockstore.alloc_max );
  fd_topob_tile_uses( topo, store_tile,  blockstore_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  fd_topob_tile_uses( topo, replay_tile, blockstore_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  fd_topob_tile_uses( topo, repair_tile, blockstore_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
  if( enable_rpc ) {
    fd_topo_tile_t * rpcserv_tile = &topo->tiles[ fd_topo_find_tile( topo, "rpcsrv", 0UL ) ];
    fd_topob_tile_uses( topo, rpcserv_tile, blockstore_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
  }

  FD_TEST( fd_pod_insertf_ulong( topo->props, blockstore_obj->id, "blockstore" ) );

  fd_topo_obj_t * runtime_pub_obj = setup_topo_runtime_pub( topo, "runtime_pub" );
  fd_topob_tile_uses( topo, replay_tile, runtime_pub_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  fd_topob_tile_uses( topo, batch_tile,  runtime_pub_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
  fd_topob_tile_uses( topo, pack_tile,  runtime_pub_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
  fd_topob_tile_uses( topo, exec_tile,  runtime_pub_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
  FD_TEST( fd_pod_insertf_ulong( topo->props, runtime_pub_obj->id, "runtime_pub" ) );

  /* Create a txncache to be used by replay. */
  fd_topo_obj_t * txncache_obj = setup_topo_txncache( topo, "tcache", FD_TXNCACHE_DEFAULT_MAX_ROOTED_SLOTS, FD_TXNCACHE_DEFAULT_MAX_LIVE_SLOTS, MAX_CACHE_TXNS_PER_SLOT );
  fd_topob_tile_uses( topo, replay_tile, txncache_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  fd_topob_tile_uses( topo, batch_tile, txncache_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );

  FD_TEST( fd_pod_insertf_ulong( topo->props, txncache_obj->id, "txncache" ) );

  for( ulong i=0UL; i<bank_tile_cnt; i++ ) {
    fd_topo_obj_t * busy_obj = fd_topob_obj( topo, "fseq", "bank_busy" );

    fd_topob_tile_uses( topo, replay_tile, busy_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
    fd_topob_tile_uses( topo, pack_tile, busy_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
    FD_TEST( fd_pod_insertf_ulong( topo->props, busy_obj->id, "bank_busy.%lu", i ) );
  }

  for( ulong i=0UL; i<exec_tile_cnt; i++ ) {
    fd_topo_obj_t * exec_spad_obj = fd_topob_obj( topo, "exec_spad", "exec_spad" );
    fd_topob_tile_uses( topo, replay_tile, exec_spad_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
    fd_topob_tile_uses( topo, exec_tile, exec_spad_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
    FD_TEST( fd_pod_insertf_ulong( topo->props, exec_spad_obj->id, "exec_spad.%lu", i ) );
  }

  for( ulong i=0UL; i<exec_tile_cnt; i++ ) {
    fd_topo_obj_t * exec_spad_obj = fd_topob_obj( topo, "fseq", "exec_fseq" );
    fd_topob_tile_uses( topo, replay_tile, exec_spad_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
    fd_topob_tile_uses( topo, exec_tile, exec_spad_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
    FD_TEST( fd_pod_insertf_ulong( topo->props, exec_spad_obj->id, "exec_fseq.%lu", i ) );
  }

  /* There's another special fseq that's used to communicate the shred
     version from the Agave boot path to the shred tile. */
  fd_topo_obj_t * poh_shred_obj = fd_topob_obj( topo, "fseq", "poh_shred" );
  fd_topo_tile_t * poh_tile = &topo->tiles[ fd_topo_find_tile( topo, "gossip", 0UL ) ];
  fd_topob_tile_uses( topo, poh_tile, poh_shred_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  fd_topob_tile_uses( topo, store_tile, poh_shred_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );

  /* This fseq maintains the node's current root slot for the purposes of
     syncing across tiles and shared data structures. */
  fd_topo_obj_t * root_slot_obj = fd_topob_obj( topo, "fseq", "root_slot" );
  fd_topob_tile_uses( topo, replay_tile, root_slot_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  fd_topob_tile_uses( topo, store_tile,  root_slot_obj, FD_SHMEM_JOIN_MODE_READ_ONLY  );
  FD_TEST( fd_pod_insertf_ulong( topo->props, root_slot_obj->id, "root_slot" ) );

  for( ulong i=0UL; i<shred_tile_cnt; i++ ) {
    fd_topo_tile_t * shred_tile = &topo->tiles[ fd_topo_find_tile( topo, "shred", i ) ];
    fd_topob_tile_uses( topo, shred_tile, poh_shred_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
  }
  FD_TEST( fd_pod_insertf_ulong( topo->props, poh_shred_obj->id, "poh_shred" ) );

  fd_topo_obj_t * poh_slot_obj = fd_topob_obj( topo, "fseq", "poh_slot" );
  fd_topob_tile_uses( topo, poh_tile, poh_slot_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  fd_topo_tile_t * sender_tile = &topo->tiles[ fd_topo_find_tile( topo, "sender", 0UL ) ];
  fd_topob_tile_uses( topo, sender_tile, poh_slot_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
  fd_topob_tile_uses( topo, replay_tile, poh_slot_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
  FD_TEST( fd_pod_insertf_ulong( topo->props, poh_slot_obj->id, "poh_slot" ) );

  fd_topo_obj_t * constipated_obj = fd_topob_obj( topo, "fseq", "constipate" );
  fd_topob_tile_uses( topo, replay_tile, constipated_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  fd_topob_tile_uses( topo, batch_tile,  constipated_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
  FD_TEST( fd_pod_insertf_ulong( topo->props, constipated_obj->id, "constipate" ) );

  if( FD_LIKELY( !is_auto_affinity ) ) {
    if( FD_UNLIKELY( affinity_tile_cnt<topo->tile_cnt ) )
      FD_LOG_ERR(( "The topology you are using has %lu tiles, but the CPU affinity specified in the config tile as [layout.affinity] only provides for %lu cores. "
                   "You should either increase the number of cores dedicated to Firedancer in the affinity string, or decrease the number of cores needed by reducing "
                   "the total tile count. You can reduce the tile count by decreasing individual tile counts in the [layout] section of the configuration file.",
                   topo->tile_cnt, affinity_tile_cnt ));
    if( FD_UNLIKELY( affinity_tile_cnt>topo->tile_cnt ) )
      FD_LOG_WARNING(( "The topology you are using has %lu tiles, but the CPU affinity specified in the config tile as [layout.affinity] provides for %lu cores. "
                       "Not all cores in the affinity will be used by Firedancer. You may wish to increase the number of tiles in the system by increasing "
                       "individual tile counts in the [layout] section of the configuration file.",
                       topo->tile_cnt, affinity_tile_cnt ));
  }

  /*                                      topo, tile_name, tile_kind_id, fseq_wksp,   link_name,      link_kind_id, reliable,            polled */
  for( ulong j=0UL; j<shred_tile_cnt; j++ )
                   fd_topos_tile_in_net(  topo,                          "metric_in", "shred_net",    j,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  for( ulong j=0UL; j<quic_tile_cnt; j++ )
                   fd_topos_tile_in_net(  topo,                          "metric_in", "quic_net",     j,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  FOR(quic_tile_cnt) for( ulong j=0UL; j<net_tile_cnt; j++ )
                       fd_topob_tile_in(  topo, "quic",    i,            "metric_in", "net_quic",     j,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  FOR(quic_tile_cnt)   fd_topob_tile_out( topo, "quic",    i,                         "quic_verify",  i                                                  );
  FOR(quic_tile_cnt)   fd_topob_tile_out( topo, "quic",    i,                         "quic_net",     i                                                  );
  /* All verify tiles read from all QUIC tiles, packets are round robin. */
  FOR(verify_tile_cnt) for( ulong j=0UL; j<quic_tile_cnt; j++ )
                       fd_topob_tile_in(  topo, "verify",  i,            "metric_in", "quic_verify",  j,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers, verify tiles may be overrun */
  FOR(verify_tile_cnt) fd_topob_tile_out( topo, "verify",  i,                         "verify_dedup", i                                                  );
  FOR(verify_tile_cnt) fd_topob_tile_in(  topo, "verify",  i,            "metric_in", "gossip_verif", 0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_in(  topo, "dedup",   0UL,          "metric_in", "voter_dedup",  0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED );
  FOR(verify_tile_cnt) fd_topob_tile_in(  topo, "dedup",   0UL,          "metric_in", "verify_dedup", i,            FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED );
  /**/                 fd_topob_tile_out( topo, "dedup",   0UL,                       "dedup_pack",   0UL                                                );
  /**/             fd_topos_tile_in_net(  topo,                          "metric_in", "gossip_net",   0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/             fd_topos_tile_in_net(  topo,                          "metric_in", "repair_net",   0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */

  FOR(shred_tile_cnt) for( ulong j=0UL; j<net_tile_cnt; j++ )
                       fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "net_shred",     j,            FD_TOPOB_UNRELIABLE,   FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "poh_shred",     0UL,          FD_TOPOB_RELIABLE,     FD_TOPOB_POLLED );
  FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "stake_out",     0UL,          FD_TOPOB_RELIABLE,     FD_TOPOB_POLLED );
  FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "crds_shred",    0UL,          FD_TOPOB_RELIABLE,     FD_TOPOB_POLLED );
  FOR(shred_tile_cnt)  fd_topob_tile_out( topo, "shred",  i,                          "shred_storei",  i                                                    );
  FOR(shred_tile_cnt)  fd_topob_tile_out( topo, "shred",  i,                          "shred_net",     i                                                    );

  /**/                 fd_topob_tile_in(  topo, "storei",  0UL,          "metric_in", "stake_out",     0UL,          FD_TOPOB_UNRELIABLE,   FD_TOPOB_POLLED );
  /**/                 fd_topob_tile_in(  topo, "storei",  0UL,          "metric_in", "repair_store",  0UL,          FD_TOPOB_RELIABLE,     FD_TOPOB_POLLED );
  /**/                 fd_topob_tile_in(  topo, "storei",  0UL,          "metric_in", "rstart_store",  0UL,          FD_TOPOB_UNRELIABLE,   FD_TOPOB_POLLED );
  FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "storei",  0UL,          "metric_in", "shred_storei",  i,            FD_TOPOB_RELIABLE,     FD_TOPOB_POLLED );
  /**/                 fd_topob_tile_out( topo, "storei",  0UL,                       "store_replay",  0UL                                                  );

  /**/                 fd_topob_tile_out( topo, "repair",  0UL,                       "repair_store",    0UL                                                );
  /**/                 fd_topob_tile_out( topo, "repair",  0UL,                       "repair_net",    0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "storei",  0UL,                       "store_repair",  0UL                                                  );
  /**/                 fd_topob_tile_in(  topo, "eqvoc",   0UL,          "metric_in", "gossip_eqvoc", 0UL,           FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */

  /* Sign links don't need to be reliable because they are synchronous,
     so there's at most one fragment in flight at a time anyway.  The
     sign links are also not polled by the mux, instead the tiles will
     read the sign responses out of band in a dedicated spin loop. */
  for( ulong i=0UL; i<shred_tile_cnt; i++ ) {
    /**/               fd_topob_tile_in(  topo, "sign",   0UL,           "metric_in", "shred_sign",    i,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
    /**/               fd_topob_tile_out( topo, "shred",  i,                          "shred_sign",    i                                                    );
    /**/               fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "sign_shred",    i,            FD_TOPOB_UNRELIABLE, FD_TOPOB_UNPOLLED );
    /**/               fd_topob_tile_out( topo, "sign",   0UL,                        "sign_shred",    i                                                    );
  }
  FOR(shred_tile_cnt)  fd_topob_tile_out( topo, "shred",  i,                          "shred_replay",  i                                                    );

  FOR(net_tile_cnt)    fd_topob_tile_in(  topo, "gossip",   0UL,          "metric_in", "net_gossip",   i,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_out( topo, "gossip",   0UL,                       "gossip_net",   0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "gossip",   0UL,                       "crds_shred",   0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "gossip",   0UL,                       "gossip_repai", 0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "gossip",   0UL,                       "gossip_verif", 0UL                                                  );
  /**/                 fd_topob_tile_in(  topo, "sign",     0UL,          "metric_in", "gossip_sign",  0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_out( topo, "gossip",   0UL,                       "gossip_sign",  0UL                                                  );
  /**/                 fd_topob_tile_in(  topo, "gossip",   0UL,          "metric_in", "voter_gossip", 0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_in(  topo, "gossip",   0UL,          "metric_in", "rstart_gossi", 0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_in(  topo, "gossip",   0UL,          "metric_in", "sign_gossip",  0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_UNPOLLED );
  /**/                 fd_topob_tile_out( topo, "sign",     0UL,                       "sign_gossip",  0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "gossip",   0UL,                       "gossip_voter", 0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "gossip",   0UL,                       "gossip_eqvoc", 0UL                                                  );

  FOR(net_tile_cnt)    fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "net_repair",    i,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "gossip_repai",  0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "stake_out",     0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "store_repair",  0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );

  /**/                 fd_topob_tile_in(  topo, "replay",  0UL,          "metric_in", "store_replay",  0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_in(  topo, "replay",  0UL,          "metric_in", "repair_repla",  0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_out( topo, "replay",  0UL,                       "stake_out",     0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "replay",  0UL,                       "replay_notif",  0UL                                                  );
  /**/                 fd_topob_tile_in(  topo, "replay",  0UL,          "metric_in", "pack_replay",   0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_in(  topo, "replay",  0UL,          "metric_in", "batch_replay",  0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_out( topo, "replay",  0UL,                       "replay_voter",  0UL                                                  );
  FOR(bank_tile_cnt)   fd_topob_tile_out( topo, "replay",  0UL,                       "replay_poh",    i                                                    );
  FOR(exec_tile_cnt)   fd_topob_tile_out( topo, "replay",  0UL,                       "replay_exec",   i                                                  ); /* TODO check order in fd_replay.c macros*/
  FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "replay",  0UL,          "metric_in", "shred_replay",  i,            FD_TOPOB_RELIABLE,     FD_TOPOB_POLLED );


  FOR(exec_tile_cnt)   fd_topob_tile_in(  topo, "exec",  i,             "metric_in", "replay_exec",  i,            FD_TOPOB_RELIABLE, FD_TOPOB_POLLED     );

  /**/                 fd_topob_tile_in(  topo, "sender",  0UL,          "metric_in",  "stake_out",    0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_in(  topo, "sender",  0UL,          "metric_in",  "gossip_voter", 0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_in(  topo, "eqvoc",   0UL,          "metric_in",  "gossip_voter", 0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_in(  topo, "sender",  0UL,          "metric_in",  "replay_voter", 0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/             fd_topos_tile_in_net(  topo,                          "metric_in",  "voter_net",    0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_out( topo, "sender",  0UL,                        "voter_net",    0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "sender",  0UL,                        "voter_gossip", 0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "sender",  0UL,                        "voter_dedup",  0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "sender",  0UL,                        "voter_sign",   0UL                                                  );
  /**/                 fd_topob_tile_in(  topo, "sign",    0UL,          "metric_in",  "voter_sign",   0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_out( topo, "sign",    0UL,                        "sign_voter",   0UL                                                  );
  /**/                 fd_topob_tile_in(  topo, "sender",  0UL,          "metric_in",  "sign_voter",   0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_UNPOLLED );

  /**/                 fd_topob_tile_in(  topo, "pack",   0UL,           "metric_in",  "dedup_pack",    0UL,         FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_in(  topo, "pack",   0UL,           "metric_in",  "poh_pack",      0UL,         FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_out( topo, "pack",   0UL,                         "pack_replay",   0UL                                                 );
  /**/                 fd_topob_tile_in(  topo, "bhole",  0UL,           "metric_in",  "replay_notif",  0UL,         FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  FOR(bank_tile_cnt)   fd_topob_tile_in(  topo, "pohi",   0UL,           "metric_in",  "replay_poh",    i,           FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_in(  topo, "pohi",   0UL,           "metric_in",  "stake_out",     0UL,         FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
  /**/                 fd_topob_tile_out( topo, "pohi",   0UL,                         "poh_shred",     0UL                                                 );

  /**/                 fd_topob_tile_in(  topo, "pohi",  0UL,            "metric_in", "pack_replay",   0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED );
                       fd_topob_tile_out( topo, "pohi",   0UL,                        "poh_pack",      0UL                                                );

  /**/                 fd_topob_tile_in(  topo, "sign",     0UL,          "metric_in", "repair_sign",  0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_out( topo, "repair",   0UL,                       "repair_sign",  0UL                                                  );
  /**/                 fd_topob_tile_in(  topo, "repair",   0UL,          "metric_in", "sign_repair",  0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_UNPOLLED );
  /**/                 fd_topob_tile_out( topo, "repair",   0UL,                      "repair_repla",  0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "sign",     0UL,                       "sign_repair",  0UL                                                  );

  FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "eqvoc",    0UL,          "metric_in", "shred_net",    i,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */

  /**/                 fd_topob_tile_out( topo, "batch",   0UL,                         "batch_replay",   0UL                                                 );

  /**/                 fd_topob_tile_out( topo, "gossip",   0UL,                       "gossi_rstart", 0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "storei",   0UL,                       "store_rstart", 0UL                                                  );
  /**/                 fd_topob_tile_in(  topo, "rstart",   0UL,          "metric_in", "gossi_rstart", 0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  /**/                 fd_topob_tile_in(  topo, "rstart",   0UL,          "metric_in", "store_rstart", 0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );

  /**/                 fd_topob_tile_out( topo, "rstart",   0UL,                       "rstart_gossi", 0UL                                                  );
  /**/                 fd_topob_tile_out( topo, "rstart",   0UL,                       "rstart_store", 0UL                                                  );

  if( enable_rpc ) {
    fd_topob_tile_in(  topo, "rpcsrv", 0UL, "metric_in",  "replay_notif", 0UL, FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
    fd_topob_tile_in(  topo, "rpcsrv", 0UL, "metric_in",  "stake_out",    0UL, FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
  }

  /* For now the only plugin consumer is the GUI */
  int plugins_enabled = config->tiles.gui.enabled;
  if( FD_LIKELY( plugins_enabled ) ) {
    fd_topob_wksp( topo, "plugin_in"    );
    fd_topob_wksp( topo, "plugin_out"   );
    fd_topob_wksp( topo, "plugin"       );

    /**/                 fd_topob_link( topo, "plugin_out",   "plugin_out",   128UL,                                    8UL+40200UL*(58UL+12UL*34UL), 1UL );
    /**/                 fd_topob_link( topo, "replay_plugi", "plugin_in",    128UL,                                    4098*8UL,               1UL );
    /**/                 fd_topob_link( topo, "gossip_plugi", "plugin_in",    128UL,                                    8UL+40200UL*(58UL+12UL*34UL), 1UL );
    /**/                 fd_topob_link( topo, "votes_plugin", "plugin_in",    128UL,                                    8UL+40200UL*(58UL+12UL*34UL), 1UL );

    /**/                 fd_topob_tile( topo, "plugin",  "plugin",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0, 0 );

    /**/                 fd_topob_tile_out( topo, "replay", 0UL,                        "replay_plugi", 0UL                                                  );
    /**/                 fd_topob_tile_out( topo, "replay", 0UL,                        "votes_plugin", 0UL                                                  );
    /**/                 fd_topob_tile_out( topo, "gossip", 0UL,                        "gossip_plugi", 0UL                                                  );
    /**/                 fd_topob_tile_out( topo, "plugin", 0UL,                        "plugin_out", 0UL                                                    );

    /**/                 fd_topob_tile_in(  topo, "plugin", 0UL,           "metric_in", "replay_plugi", 0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED );
    /**/                 fd_topob_tile_in(  topo, "plugin", 0UL,           "metric_in", "gossip_plugi", 0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED );
    /**/                 fd_topob_tile_in(  topo, "plugin", 0UL,           "metric_in", "stake_out",    0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED );
    /**/                 fd_topob_tile_in(  topo, "plugin", 0UL,           "metric_in", "votes_plugin", 0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED );
  }

  if( FD_LIKELY( config->tiles.gui.enabled ) ) {
    fd_topob_wksp( topo, "gui"          );
    /**/                 fd_topob_tile(     topo, "gui",     "gui",     "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0, 1 );
    /**/                 fd_topob_tile_in(  topo, "gui",    0UL,        "metric_in",     "plugin_out",   0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED );
  }

  FOR(net_tile_cnt) fd_topos_net_tile_finish( topo, i );

  for( ulong i=0UL; i<topo->tile_cnt; i++ ) {
    fd_topo_tile_t * tile = &topo->tiles[ i ];

    if( FD_UNLIKELY( !strcmp( tile->name, "net" ) || !strcmp( tile->name, "sock" ) ) ) {

      tile->net.shred_listen_port              = config->tiles.shred.shred_listen_port;
      tile->net.quic_transaction_listen_port   = config->tiles.quic.quic_transaction_listen_port;
      tile->net.legacy_transaction_listen_port = config->tiles.quic.regular_transaction_listen_port;
      tile->net.gossip_listen_port             = config->gossip.port;
      tile->net.repair_intake_listen_port      = config->tiles.repair.repair_intake_listen_port;
      tile->net.repair_serve_listen_port       = config->tiles.repair.repair_serve_listen_port;

    } else if( FD_UNLIKELY( !strcmp( tile->name, "netlnk" ) ) ) {

      /* already configured */

    } else if( FD_UNLIKELY( !strcmp( tile->name, "quic" ) ) ) {

      tile->quic.reasm_cnt                      = config->tiles.quic.txn_reassembly_count;
      tile->quic.out_depth                      = config->tiles.verify.receive_buffer_size;
      tile->quic.max_concurrent_connections     = config->tiles.quic.max_concurrent_connections;
      tile->quic.max_concurrent_handshakes      = config->tiles.quic.max_concurrent_handshakes;
      tile->quic.quic_transaction_listen_port   = config->tiles.quic.quic_transaction_listen_port;
      tile->quic.idle_timeout_millis            = config->tiles.quic.idle_timeout_millis;
      tile->quic.ack_delay_millis               = config->tiles.quic.ack_delay_millis;
      tile->quic.retry                          = config->tiles.quic.retry;

    } else if( FD_UNLIKELY( !strcmp( tile->name, "verify" ) ) ) {
      tile->verify.tcache_depth = config->tiles.verify.signature_cache_size;

    } else if( FD_UNLIKELY( !strcmp( tile->name, "dedup" ) ) ) {
      tile->dedup.tcache_depth = config->tiles.dedup.signature_cache_size;

    } else if( FD_UNLIKELY( !strcmp( tile->name, "shred" ) ) ) {
      strncpy( tile->shred.identity_key_path, config->consensus.identity_path, sizeof(tile->shred.identity_key_path) );

      tile->shred.depth                         = topo->links[ tile->out_link_id[ 0 ] ].depth;
      tile->shred.fec_resolver_depth            = config->tiles.shred.max_pending_shred_sets;
      tile->shred.expected_shred_version        = config->consensus.expected_shred_version;
      tile->shred.shred_listen_port             = config->tiles.shred.shred_listen_port;
      tile->shred.larger_shred_limits_per_block = config->development.bench.larger_shred_limits_per_block;

    } else if( FD_UNLIKELY( !strcmp( tile->name, "storei" ) ) ) {
      strncpy( tile->store_int.blockstore_file, config->blockstore.file, sizeof(tile->store_int.blockstore_file) );
      strncpy( tile->store_int.blockstore_restore, config->blockstore.restore, sizeof(tile->store_int.blockstore_restore) );
      strncpy( tile->store_int.identity_key_path, config->consensus.identity_path, sizeof(tile->store_int.identity_key_path) );
      strncpy( tile->store_int.slots_pending, config->tiles.store_int.slots_pending, sizeof( tile->store_int.slots_pending ) );
      strncpy( tile->store_int.shred_cap_archive, config->tiles.store_int.shred_cap_archive, sizeof(tile->store_int.shred_cap_archive) );
      strncpy( tile->store_int.shred_cap_replay, config->tiles.store_int.shred_cap_replay, sizeof(tile->store_int.shred_cap_replay) );
      tile->store_int.shred_cap_end_slot     = config->tiles.store_int.shred_cap_end_slot;
      tile->store_int.expected_shred_version = config->consensus.expected_shred_version;

    } else if( FD_UNLIKELY( !strcmp( tile->name, "gossip" ) ) ) {
      tile->gossip.ip_addr = config->tiles.net.ip_addr;
      strncpy( tile->gossip.identity_key_path, config->consensus.identity_path, sizeof(tile->gossip.identity_key_path) );
      tile->gossip.gossip_listen_port =  config->gossip.port;
      tile->gossip.tvu_port = config->tiles.shred.shred_listen_port;
      if( FD_UNLIKELY( tile->gossip.tvu_port>(ushort)(USHORT_MAX-6) ) )
        FD_LOG_ERR(( "shred_listen_port in the config must not be greater than %hu", (ushort)(USHORT_MAX-6) ));
      tile->gossip.expected_shred_version = config->consensus.expected_shred_version;
      tile->gossip.tpu_port             = config->tiles.quic.regular_transaction_listen_port;
      tile->gossip.tpu_quic_port        = config->tiles.quic.quic_transaction_listen_port;
      tile->gossip.tpu_vote_port        = config->tiles.quic.regular_transaction_listen_port; /* TODO: support separate port for tpu vote */
      tile->gossip.repair_serve_port    = config->tiles.repair.repair_serve_listen_port;
      tile->gossip.entrypoints_cnt      = fd_ulong_min( config->gossip.resolved_entrypoints_cnt, FD_TOPO_GOSSIP_ENTRYPOINTS_MAX );
      fd_memcpy( tile->gossip.entrypoints, config->gossip.resolved_entrypoints, tile->gossip.entrypoints_cnt * sizeof(fd_ip4_port_t) );

    } else if( FD_UNLIKELY( !strcmp( tile->name, "repair" ) ) ) {
      tile->repair.repair_intake_listen_port =  config->tiles.repair.repair_intake_listen_port;
      tile->repair.repair_serve_listen_port =   config->tiles.repair.repair_serve_listen_port;
      strncpy( tile->repair.good_peer_cache_file, config->tiles.repair.good_peer_cache_file, sizeof(tile->repair.good_peer_cache_file) );

      strncpy( tile->repair.identity_key_path, config->consensus.identity_path, sizeof(tile->repair.identity_key_path) );

    } else if( FD_UNLIKELY( !strcmp( tile->name, "replay" ) )) {

      tile->replay.fec_max = config->tiles.shred.max_pending_shred_sets;

      /* specified by [tiles.replay] */

      strncpy( tile->replay.blockstore_file, config->blockstore.file, sizeof(tile->replay.blockstore_file) );
      strncpy( tile->replay.blockstore_checkpt, config->blockstore.checkpt, sizeof(tile->replay.blockstore_checkpt) );

      tile->replay.tx_metadata_storage = config->rpc.extended_tx_metadata_storage;
      strncpy( tile->replay.capture, config->tiles.replay.capture, sizeof(tile->replay.capture) );
      strncpy( tile->replay.funk_checkpt, config->tiles.replay.funk_checkpt, sizeof(tile->replay.funk_checkpt) );
      tile->replay.funk_rec_max = config->tiles.replay.funk_rec_max;
      tile->replay.funk_sz_gb   = config->tiles.replay.funk_sz_gb;
      tile->replay.funk_txn_max = config->tiles.replay.funk_txn_max;
      strncpy( tile->replay.funk_file, config->tiles.replay.funk_file, sizeof(tile->replay.funk_file) );
      tile->replay.plugins_enabled = plugins_enabled;

      if( FD_UNLIKELY( !strncmp( config->tiles.replay.genesis,  "", 1 )
                    && !strncmp( config->tiles.replay.snapshot, "", 1 ) ) ) {
        fd_cstr_printf_check(  config->tiles.replay.genesis, PATH_MAX, NULL, "%s/genesis.bin", config->ledger.path );
      }
      strncpy( tile->replay.genesis, config->tiles.replay.genesis, sizeof(tile->replay.genesis) );

      strncpy( tile->replay.incremental, config->tiles.replay.incremental, sizeof(tile->replay.incremental) );
      strncpy( tile->replay.slots_replayed, config->tiles.replay.slots_replayed, sizeof(tile->replay.slots_replayed) );
      strncpy( tile->replay.snapshot, config->tiles.replay.snapshot, sizeof(tile->replay.snapshot) );
      strncpy( tile->replay.status_cache, config->tiles.replay.status_cache, sizeof(tile->replay.status_cache) );
      tile->replay.tpool_thread_count = config->tiles.replay.tpool_thread_count;
      if( FD_UNLIKELY( tile->replay.tpool_thread_count == 0 || tile->replay.tpool_thread_count>=FD_TILE_MAX-1 ) ) {
        FD_LOG_ERR(( "bad tpool_thread_count %lu", tile->replay.tpool_thread_count ));
      }
      strncpy( tile->replay.cluster_version, config->tiles.replay.cluster_version, sizeof(tile->replay.cluster_version) );
      tile->replay.bank_tile_count = config->layout.bank_tile_count;
      tile->replay.exec_tile_count = config->layout.exec_tile_count;
      strncpy( tile->replay.tower_checkpt, config->tiles.replay.tower_checkpt, sizeof(tile->replay.tower_checkpt) );

      /* not specified by [tiles.replay] */

      strncpy( tile->replay.identity_key_path, config->consensus.identity_path, sizeof(tile->replay.identity_key_path) );
      tile->replay.ip_addr = config->tiles.net.ip_addr;
      tile->replay.vote = config->consensus.vote;
      strncpy( tile->replay.vote_account_path, config->consensus.vote_account_path, sizeof(tile->replay.vote_account_path) );
      tile->replay.full_interval        = config->tiles.batch.full_interval;
      tile->replay.incremental_interval = config->tiles.batch.incremental_interval;

    } else if( FD_UNLIKELY( !strcmp( tile->name, "bhole" ) ) ) {

    } else if( FD_UNLIKELY( !strcmp( tile->name, "sign" ) ) ) {
      strncpy( tile->sign.identity_key_path, config->consensus.identity_path, sizeof(tile->sign.identity_key_path) );

    } else if( FD_UNLIKELY( !strcmp( tile->name, "metric" ) ) ) {
      if( FD_UNLIKELY( !fd_cstr_to_ip4_addr( config->tiles.metric.prometheus_listen_address, &tile->metric.prometheus_listen_addr ) ) )
        FD_LOG_ERR(( "failed to parse prometheus listen address `%s`", config->tiles.metric.prometheus_listen_address ));
      tile->metric.prometheus_listen_port = config->tiles.metric.prometheus_listen_port;

    } else if( FD_UNLIKELY( !strcmp( tile->name, "rtpool" ) ) ) {
      /* Nothing for now */
    } else if( FD_UNLIKELY( !strcmp( tile->name, "pack" ) ) ) {
      tile->pack.max_pending_transactions      = config->tiles.pack.max_pending_transactions;
      tile->pack.bank_tile_count               = config->layout.bank_tile_count;
      tile->pack.larger_max_cost_per_block     = config->development.bench.larger_max_cost_per_block;
      tile->pack.larger_shred_limits_per_block = config->development.bench.larger_shred_limits_per_block;
      tile->pack.use_consumed_cus              = config->tiles.pack.use_consumed_cus;
      if( FD_UNLIKELY( tile->pack.use_consumed_cus ) ) FD_LOG_ERR(( "Firedancer does not support CU rebating yet.  [tiles.pack.use_consumed_cus] must be false" ));
    } else if( FD_UNLIKELY( !strcmp( tile->name, "pohi" ) ) ) {
      strncpy( tile->poh.identity_key_path, config->consensus.identity_path, sizeof(tile->poh.identity_key_path) );

      tile->poh.bank_cnt = config->layout.bank_tile_count;
    } else if( FD_UNLIKELY( !strcmp( tile->name, "sender" ) ) ) {
      tile->sender.tpu_listen_port = config->tiles.quic.regular_transaction_listen_port;
      tile->sender.ip_addr = config->tiles.net.ip_addr;

      strncpy( tile->sender.identity_key_path, config->consensus.identity_path, sizeof(tile->sender.identity_key_path) );
    } else if( FD_UNLIKELY( !strcmp( tile->name, "eqvoc" ) ) ) {
      strncpy( tile->eqvoc.identity_key_path, config->consensus.identity_path, sizeof(tile->eqvoc.identity_key_path) );
    } else if( FD_UNLIKELY( !strcmp( tile->name, "rpcsrv" ) ) ) {
      strncpy( tile->replay.blockstore_file, config->blockstore.file, sizeof(tile->replay.blockstore_file) );
      tile->replay.funk_rec_max = config->tiles.replay.funk_rec_max;
      tile->replay.funk_sz_gb   = config->tiles.replay.funk_sz_gb;
      tile->replay.funk_txn_max = config->tiles.replay.funk_txn_max;
      strncpy( tile->replay.funk_file, config->tiles.replay.funk_file, sizeof(tile->replay.funk_file) );
      tile->rpcserv.rpc_port = config->rpc.port;
      tile->rpcserv.tpu_port = config->tiles.quic.regular_transaction_listen_port;
      tile->rpcserv.tpu_ip_addr = config->tiles.net.ip_addr;
      strncpy( tile->rpcserv.identity_key_path, config->consensus.identity_path, sizeof(tile->rpcserv.identity_key_path) );
    } else if( FD_UNLIKELY( !strcmp( tile->name, "batch" ) ) ) {
      tile->batch.full_interval        = config->tiles.batch.full_interval;
      tile->batch.incremental_interval = config->tiles.batch.incremental_interval;
      strncpy( tile->batch.out_dir, config->tiles.batch.out_dir, sizeof(tile->batch.out_dir) );
      tile->batch.hash_tpool_thread_count = config->tiles.batch.hash_tpool_thread_count;
      strncpy( tile->replay.funk_file, config->tiles.replay.funk_file, sizeof(tile->replay.funk_file) );
    } else if( FD_UNLIKELY( !strcmp( tile->name, "btpool" ) ) ) {
      /* Nothing for now */
    } else if( FD_UNLIKELY( !strcmp( tile->name, "gui" ) ) ) {
      if( FD_UNLIKELY( !fd_cstr_to_ip4_addr( config->tiles.gui.gui_listen_address, &tile->gui.listen_addr ) ) )
        FD_LOG_ERR(( "failed to parse gui listen address `%s`", config->tiles.gui.gui_listen_address ));
      tile->gui.listen_port = config->tiles.gui.gui_listen_port;
      tile->gui.is_voting = strcmp( config->consensus.vote_account_path, "" );
      strncpy( tile->gui.cluster, config->cluster, sizeof(tile->gui.cluster) );
      strncpy( tile->gui.identity_key_path, config->consensus.identity_path, sizeof(tile->gui.identity_key_path) );
      tile->gui.max_http_connections      = config->tiles.gui.max_http_connections;
      tile->gui.max_websocket_connections = config->tiles.gui.max_websocket_connections;
      tile->gui.max_http_request_length   = config->tiles.gui.max_http_request_length;
      tile->gui.send_buffer_size_mb       = config->tiles.gui.send_buffer_size_mb;
    } else if( FD_UNLIKELY( !strcmp( tile->name, "plugin" ) ) ) {

    } else if( FD_UNLIKELY( !strcmp( tile->name, "exec" ) ) ) {
      strncpy( tile->exec.funk_file, config->tiles.replay.funk_file, sizeof(tile->exec.funk_file) );
    } else if( FD_UNLIKELY( !strcmp( tile->name, "rstart" ) ) ) {
      tile->restart.in_wen_restart = config->tiles.restart.in_wen_restart;
      strncpy( tile->restart.funk_file, config->tiles.replay.funk_file, sizeof(tile->replay.funk_file) );
      strncpy( tile->restart.tower_checkpt, config->tiles.replay.tower_checkpt, sizeof(tile->replay.tower_checkpt) );
      strncpy( tile->restart.identity_key_path, config->consensus.identity_path, sizeof(tile->restart.identity_key_path) );
      fd_memcpy( tile->restart.genesis_hash, config->tiles.restart.genesis_hash, FD_BASE58_ENCODED_32_SZ );
      fd_memcpy( tile->restart.restart_coordinator, config->tiles.restart.wen_restart_coordinator, FD_BASE58_ENCODED_32_SZ );
    } else {
      FD_LOG_ERR(( "unknown tile name %lu `%s`", i, tile->name ));
    }
  }

  if( FD_UNLIKELY( is_auto_affinity ) ) fd_topob_auto_layout( topo );

  fd_topob_finish( topo, fdctl_obj_align, fdctl_obj_footprint, fdctl_obj_loose );

  const char * status_cache = config->tiles.replay.status_cache;
  if ( strlen( status_cache ) > 0 ) {
    /* Make the status cache workspace match the parameters used to create the
       checkpoint. This is a bit nonintuitive because of the way
       fd_topo_create_workspace works. */
    fd_wksp_preview_t preview[1];
    int err = fd_wksp_preview( status_cache, preview );
    if( FD_UNLIKELY( err ) ) FD_LOG_ERR(( "unable to preview %s: error %d", status_cache, err ));
    fd_topo_wksp_t * wksp = &topo->workspaces[ topo->objs[ txncache_obj->id ].wksp_id ];
    wksp->part_max = preview->part_max;
    wksp->known_footprint = 0;
    wksp->total_footprint = preview->data_max;
    ulong page_sz = FD_SHMEM_GIGANTIC_PAGE_SZ;
    wksp->page_sz = page_sz;
    ulong footprint = fd_wksp_footprint( preview->part_max, preview->data_max );
    wksp->page_cnt = footprint / page_sz;
  }

  config->topo = *topo;
}
