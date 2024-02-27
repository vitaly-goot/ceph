//#include "gperftools/profiler.h"
#include <iostream>
#include <math.h>
#include <fstream>
#include <rados/librados.h>
#include <vector>
#include <iomanip>

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

void hexDump(const void* data, size_t size) {
    const unsigned char* byteData = static_cast<const unsigned char*>(data);
    size_t i = 0;
    for (size_t i = 0; i < size; ++i) {
        if (isprint(byteData[i])) 
            cout << byteData[i];
        else  
            cout << "\\x" << setw(2) << setfill('0') << hex << (int)byteData[i];
    }
    cout << endl;
}

void scanObject(const char * objectname)
{
    cout << "Object: " << objectname << endl;
    rados_t cluster;
    rados_ioctx_t io;
    char cluster_name[] = "ceph";
    char user_name[] = "client.admin";
    uint64_t flags = 0;

    // Initialize the RADOS cluster handle
    rados_create2(&cluster, cluster_name, user_name, flags);
    rados_conf_read_file(cluster, "/etc/ceph/ceph.conf");
    rados_connect(cluster);

    // Create an I/O context for the pool
    rados_ioctx_create(cluster, "default.rgw.buckets.index", &io);

    // Start the read operation
    rados_read_op_t read_op = rados_create_read_op();

    string start_after = ""; 
    size_t max_return = 1000;   
    rados_omap_iter_t iter_keys;
    unsigned char more = 1;
    int prval;
    char *key, *val;
    size_t key_len, val_len;

    while (more) {
        rados_read_op_omap_get_vals2(read_op, start_after.c_str(), "", max_return, &iter_keys, &more, &prval);

        // Perform the read operation on the object
        int ret = rados_read_op_operate(read_op, io, objectname, 0);
        if (ret < 0) {
            fprintf(stderr, "Failed to read keys: %s\n", strerror(-ret));
            break;
        }

        while (rados_omap_get_next2(iter_keys, &key, &val, &key_len, &val_len) == 0 && key) {
          string packed_key(key, key_len);
          if (packed_key.find('\0') != std::string::npos) {
            printf("key_len: %d val_len %d\n", (int)key_len, int(val_len));
            hexDump(key, key_len);
          }  
          start_after = packed_key;
        }


        // Reset the read operation for next iteration
        rados_release_read_op(read_op);
        read_op = rados_create_read_op();
    }

    // Cleanup
    rados_release_read_op(read_op);
    rados_ioctx_destroy(io);
    rados_shutdown(cluster);
}

void scan(string & bucket_id, int nshards)
{
  for (int i = 0; i < nshards; ++i) {
    cout << "Scanning shard " << to_string(i) << " =======> ";
    string shard = ".dir." + bucket_id + "." + to_string(i);
    scanObject(shard.c_str());
  }
}

void rmomapkey(const char * bucket_id, vector<char> key)
{
  rados_t cluster;
  char cluster_name[] = "ceph";
  char user_name[] = "client.admin";
  uint64_t flags = 0;

  int ret = rados_create2(&cluster, cluster_name, user_name, flags);
  if (ret < 0) {
    fprintf(stderr, "cannot create a cluster handle: %s\n", strerror(-ret));
    exit(1);
  }
  ret = rados_conf_read_file(cluster, "/etc/ceph/ceph.conf");
  if (ret < 0) {
    fprintf(stderr, "cannot read config file: %s\n", strerror(-ret));
    exit(1);
  }
  ret = rados_connect(cluster);
  if (ret < 0) {
    fprintf(stderr, "cannot connect to cluster: %s\n", strerror(-ret));
    exit(1);
  }

  rados_ioctx_t io;
  ret = rados_ioctx_create(cluster, "default.rgw.buckets.index", &io);
  if (ret < 0) {
    fprintf(stderr, "cannot open rados pool default.rgw.buckets.index: %s\n", strerror(-ret));
    rados_shutdown(cluster);
    exit(1);
  }

  const char *keys[] = {key.data()};
  size_t key_lens[] = {key.size()};

  rados_omap_iter_t iter_vals_by_key;
  int r_vals_by_key;
  rados_read_op_t read_op = rados_create_read_op();
  rados_read_op_omap_get_vals_by_keys2(read_op, (char* const*)keys, 1, (const size_t*)&key_lens, &iter_vals_by_key, &r_vals_by_key);
  rados_read_op_operate(read_op, io, bucket_id, 0);
  rados_release_read_op(read_op);
  size_t len = rados_omap_iter_size(iter_vals_by_key);
  if (len == 1) {
    rados_write_op_t write_op = rados_create_write_op();
    rados_write_op_omap_rm_keys2(write_op, (const char * const *)keys, (const size_t*)&key_lens, 1);
    printf("Key %s size %d\n", key.data(), (int)key.size());
    ret = rados_write_op_operate(write_op, io, bucket_id, NULL, flags);
    if (ret < 0) {
      fprintf(stderr, "Failed to remove keys: %s\n", strerror(-ret));
    } else {
      printf("Keys removed successfully.\n");
    }
    rados_release_write_op(write_op);
  } 
  else {
      printf("Keys does not exist  .\n");
  }
  printf("Done\n");
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
  if (file_name == "--scan") {
    cout << "Running in scan mode\n";
    scan(bucket_id, num_shards);
    exit(0);
  }

  ifstream file(file_name, ios::binary);
  if (!file.is_open()) {
    cerr << "Error opening file: " + file_name << endl;
    return 1;
  }
  vector<char> buffer((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());

  uint32_t sid = ceph_str_hash_linux(buffer.data(), (int)buffer.size());
  uint32_t sid2 = sid ^ ((sid & 0xFF) << 24);
  uint32_t shard_index = rgw_shards_mod(sid2, num_shards);
  bucket_id = ".dir." + bucket_id + "." + to_string(shard_index);
  printf("%s %d %d\n", bucket_id.c_str(), sid2, shard_index);

  rmomapkey(bucket_id.c_str(), buffer);

  return 0;
}
