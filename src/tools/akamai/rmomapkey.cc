//#include "gperftools/profiler.h"
#include <iostream>
#include <math.h>
#include <fstream>
#include <rados/librados.h>
#include <vector>

#define RGW_SHARDS_PRIME_0 7877
#define RGW_SHARDS_PRIME_1 65521

using namespace std;

unsigned ceph_str_hash_linux(const char *str, unsigned length)
{
  unsigned hash = 0;

  while (length--) {
    unsigned char c = *str++;
    hash = (hash + (c << 4) + (c >> 4)) * 11;
  }
  return hash;
}

// only called by rgw_shard_id and rgw_bucket_shard_index
static inline int rgw_shards_mod(unsigned hval, int max_shards)
{
  if (max_shards <= RGW_SHARDS_PRIME_0) {
    return hval % RGW_SHARDS_PRIME_0 % max_shards;
  }
  return hval % RGW_SHARDS_PRIME_1 % max_shards;
}

void rmomapkey(const char * bucket_id, vector<char> key)
{
  rados_t cluster;
  char cluster_name[] = "ceph";
  char user_name[] = "client.admin";
  uint64_t flags = 0;

  rados_create2(&cluster, cluster_name, user_name, flags);
  rados_conf_read_file(cluster, "/etc/ceph/ceph.conf");
  rados_connect(cluster);

  rados_ioctx_t io;
  rados_ioctx_create(cluster, "default.rgw.buckets.index", &io);

  const char *keys[] = {key.data()};
  size_t key_lens[] = {key.size()};

  rados_write_op_t write_op = rados_create_write_op();
  rados_write_op_omap_rm_keys2(write_op, (const char * const *)keys, (const size_t*)&key_lens, 1);
  int ret = rados_write_op_operate(write_op, io, bucket_id, NULL, flags);
  if (ret < 0) {
    fprintf(stderr, "Failed to remove keys: %s\n", strerror(-ret));
  } else {
    printf("Keys removed successfully.\n");
  }

  rados_write_op_remove(write_op);
  rados_ioctx_destroy(io);
  rados_shutdown(cluster);
}

int main(int argc, char* argv[])
{
  if (argc < 4) {
    cerr << "Usage: " << argv[0] << " bucket_id num_shards file\n";
    return 1;
  }
  string bucket_id = argv[1];
  size_t num_shards =  atoi(argv[2]);
  string file_name =  argv[3];

  ifstream file(file_name, ios::binary);
  if (!file.is_open()) {
    cerr << "Error opening file: " + file_name << endl;
    return 1;
  }
  vector<char> buffer((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());

  uint32_t sid = ceph_str_hash_linux(buffer.data(), (int)buffer.size());
  uint32_t sid2 = sid ^ ((sid & 0xFF) << 24);
  uint32_t shard_index = rgw_shards_mod(sid2, num_shards);
  bucket_id += "." + to_string(shard_index);

  rmomapkey(bucket_id.c_str(), buffer);

  //printf("%d %d\n", sid2, shard_index);
  return 0;
}
