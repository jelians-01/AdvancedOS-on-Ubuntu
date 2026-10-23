#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { FAT32_EOC = 0x0ffffff8u, FAT32_BAD = 0x0ffffff7u,
       FAT32_RESERVED = 0x0ffffff0u, MAX_SECTOR_SIZE = 4096 };

typedef struct {
  int fd;
  uint64_t image_bytes;
  uint32_t bytes_per_sector;
  uint32_t sectors_per_cluster;
  uint32_t cluster_bytes;
  uint32_t reserved_sectors;
  uint32_t fat_count;
  uint32_t fat_sectors;
  uint32_t total_sectors;
  uint32_t root_cluster;
  uint32_t cluster_count;
  uint32_t max_cluster;
  uint32_t active_fat;
  bool mirrored;
  uint64_t fat_offset;
  uint64_t data_offset;
  char error[256];
} FatFs;

typedef struct {
  char name[53];
  uint8_t attributes;
  uint32_t first_cluster;
  uint32_t size;
  uint64_t entry_offset;
} FatEntry;

typedef int (*ChunkFn)(const uint8_t *bytes, size_t length, void *context);

static uint16_t le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int set_error(FatFs *fs, const char *message) {
  (void)snprintf(fs->error, sizeof(fs->error), "%s", message);
  return -1;
}

static int read_exact(FatFs *fs, uint64_t offset, void *buffer, size_t length) {
  uint8_t *out = buffer;
  size_t done = 0;
  if (offset > fs->image_bytes || (uint64_t)length > fs->image_bytes - offset)
    return set_error(fs, "read extends beyond the image");
  while (done < length) {
    ssize_t n = pread(fs->fd, out + done, length - done,
                      (off_t)(offset + done));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return set_error(fs, "short read from image");
    done += (size_t)n;
  }
  return 0;
}

static bool power_of_two(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static int fat_open(FatFs *fs, const char *path) {
  struct stat st;
  uint8_t boot[512];
  uint32_t total16, fat16, total32, fat32, root_entries;
  uint32_t data_sectors, ext_flags;
  uint64_t overhead_sectors, fat_bytes;
  uint64_t fat_entries;
  uint64_t volume_bytes;
  memset(fs, 0, sizeof(*fs));
  fs->fd = -1;
  fs->fd = open(path, O_RDONLY);
  if (fs->fd < 0) {
    (void)snprintf(fs->error, sizeof(fs->error), "cannot open '%s': %s",
                   path, strerror(errno));
    return -1;
  }
  if (fstat(fs->fd, &st) != 0 || st.st_size < 0) {
    (void)snprintf(fs->error, sizeof(fs->error), "cannot stat image: %s",
                   strerror(errno));
    return -1;
  }
  fs->image_bytes = (uint64_t)st.st_size;
  if (fs->image_bytes < sizeof(boot))
    return set_error(fs, "image is shorter than one boot sector");
  if (read_exact(fs, 0, boot, sizeof(boot)) < 0)
    return -1;
  if (boot[510] != 0x55 || boot[511] != 0xaa)
    return set_error(fs, "missing 0x55aa boot-sector signature");

  fs->bytes_per_sector = le16(boot + 11);
  fs->sectors_per_cluster = boot[13];
  fs->reserved_sectors = le16(boot + 14);
  fs->fat_count = boot[16];
  root_entries = le16(boot + 17);
  total16 = le16(boot + 19);
  fat16 = le16(boot + 22);
  total32 = le32(boot + 32);
  fat32 = le32(boot + 36);
  ext_flags = le16(boot + 40);
  if (le16(boot + 42) != 0)
    return set_error(fs, "unsupported FAT32 version");
  if (ext_flags & 0xff70u)
    return set_error(fs, "unsupported reserved ExtFlags bits");
  fs->root_cluster = le32(boot + 44) & 0x0fffffffu;

  if (!power_of_two(fs->bytes_per_sector) || fs->bytes_per_sector < 512 ||
      fs->bytes_per_sector > MAX_SECTOR_SIZE)
    return set_error(fs, "unsupported bytes-per-sector value (expected 512..4096 power of two)");
  if (!power_of_two(fs->sectors_per_cluster) || fs->sectors_per_cluster > 128)
    return set_error(fs, "invalid sectors-per-cluster value");
  if (fs->reserved_sectors == 0 || fs->fat_count == 0)
    return set_error(fs, "reserved-sector or FAT count is zero");
  if (root_entries != 0 || fat16 != 0 || fat32 == 0)
    return set_error(fs, "BPB does not describe a FAT32 volume");
  if (le32(boot + 28) != 0)
    return set_error(fs, "non-zero hidden sectors are unsupported for raw superfloppy images");
  if (total16 != 0 && total32 != 0)
    return set_error(fs, "both 16-bit and 32-bit total-sector fields are set");
  fs->total_sectors = total16 != 0 ? total16 : total32;
  fs->fat_sectors = fat32;
  if (fs->total_sectors == 0)
    return set_error(fs, "total-sector count is zero");
  if ((ext_flags & 0x0080u) != 0) {
    fs->active_fat = ext_flags & 0x000fu;
    if (fs->active_fat >= fs->fat_count)
      return set_error(fs, "active FAT index is outside the FAT table count");
  }
  fs->mirrored = (ext_flags & 0x0080u) == 0;
  volume_bytes = (uint64_t)fs->total_sectors * fs->bytes_per_sector;
  if (volume_bytes > fs->image_bytes)
    return set_error(fs, "image is truncated before the BPB volume ends");
  overhead_sectors = (uint64_t)fs->reserved_sectors +
                     (uint64_t)fs->fat_count * fs->fat_sectors;
  if (overhead_sectors >= fs->total_sectors)
    return set_error(fs, "reserved sectors and FATs leave no data region");
  data_sectors = (uint32_t)(fs->total_sectors - overhead_sectors);
  fs->cluster_count = data_sectors / fs->sectors_per_cluster;
  /* FAT32's data-cluster range must not overlap reserved FAT values. */
  if (fs->cluster_count < 65525 || fs->cluster_count > 0x0fffffeeu)
    return set_error(fs, "cluster count is outside the FAT32 range");
  fs->max_cluster = fs->cluster_count + 1;
  if (fs->root_cluster < 2 || fs->root_cluster > fs->max_cluster)
    return set_error(fs, "root-directory cluster is outside the data region");
  fs->cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
  fat_bytes = (uint64_t)fs->fat_sectors * fs->bytes_per_sector;
  fat_entries = fat_bytes / 4;
  if (fat_entries < fs->cluster_count + 2)
    return set_error(fs, "FAT is too small to describe all data clusters");
  fs->fat_offset = ((uint64_t)fs->reserved_sectors +
                    (uint64_t)fs->active_fat * fs->fat_sectors) *
                   fs->bytes_per_sector;
  fs->data_offset = ((uint64_t)fs->reserved_sectors +
                     (uint64_t)fs->fat_count * fs->fat_sectors) *
                    fs->bytes_per_sector;
  return 0;
}

static void fat_close(FatFs *fs) {
  if (fs->fd >= 0)
    (void)close(fs->fd);
  fs->fd = -1;
}

/* Returns 1 for a data-cluster link, 0 for EOC, and -1 for corrupt FAT data. */
static int fat_next(FatFs *fs, uint32_t cluster, uint32_t *next) {
  uint8_t raw[4];
  uint32_t value;
  if (cluster < 2 || cluster > fs->max_cluster)
    return set_error(fs, "cluster number is outside the data region");
  if (read_exact(fs, fs->fat_offset + (uint64_t)cluster * 4, raw, sizeof(raw)) < 0)
    return -1;
  value = le32(raw) & 0x0fffffffu;
  if (fs->mirrored) {
    for (uint32_t i = 1; i < fs->fat_count; ++i) {
      uint64_t offset = fs->fat_offset +
                        (uint64_t)i * fs->fat_sectors * fs->bytes_per_sector;
      if (read_exact(fs, offset + (uint64_t)cluster * 4, raw, sizeof(raw)) < 0)
        return -1;
      if ((le32(raw) & 0x0fffffffu) != value)
        return set_error(fs, "mirrored FAT entries disagree");
    }
  }
  if (value >= FAT32_EOC)
    return 0;
  if (value == FAT32_BAD || value >= FAT32_RESERVED || value < 2 ||
      value > fs->max_cluster)
    return set_error(fs, "FAT chain contains a free, reserved, bad, or out-of-range link");
  *next = value;
  return 1;
}

static int cluster_offset(FatFs *fs, uint32_t cluster, uint64_t *offset) {
  if (cluster < 2 || cluster > fs->max_cluster)
    return set_error(fs, "cluster number is outside the data region");
  *offset = fs->data_offset + (uint64_t)(cluster - 2) * fs->cluster_bytes;
  if (*offset > (uint64_t)fs->total_sectors * fs->bytes_per_sector ||
      fs->cluster_bytes >
          (uint64_t)fs->total_sectors * fs->bytes_per_sector - *offset)
    return set_error(fs, "cluster data extends beyond the BPB volume");
  return 0;
}

/* Validate the complete chain before exposing bytes. The bitmap bounds both
 * memory use and corrupt-chain traversal by the declared volume geometry. */
static int collect_chain(FatFs *fs, uint32_t start, uint32_t **chain, size_t *count) {
  uint8_t *seen = calloc(((size_t)fs->max_cluster + 8) / 8, 1);
  uint32_t *items = NULL, current = start;
  size_t used = 0, capacity = 0;
  int result = -1;
  *chain = NULL;
  *count = 0;
  if (!seen)
    return set_error(fs, "out of memory validating chain");
  for (;;) {
    if (current < 2 || current > fs->max_cluster) {
      set_error(fs, "cluster number is outside the data region");
      break;
    }
    if (seen[current / 8] & (1u << (current % 8))) {
      set_error(fs, "cluster chain contains a cycle");
      break;
    }
    seen[current / 8] |= (uint8_t)(1u << (current % 8));
    if (used == capacity) {
      size_t next_capacity = capacity ? capacity * 2 : 16;
      if (next_capacity > fs->cluster_count)
        next_capacity = fs->cluster_count;
      uint32_t *grown = realloc(items, next_capacity * sizeof(*items));
      if (!grown) {
        set_error(fs, "out of memory collecting chain");
        break;
      }
      items = grown;
      capacity = next_capacity;
    }
    items[used++] = current;
    uint32_t next = 0;
    int status = fat_next(fs, current, &next);
    if (status < 0)
      break;
    if (status == 0) {
      *chain = items;
      *count = used;
      result = 0;
      break;
    }
    current = next;
  }
  free(seen);
  if (result < 0)
    free(items);
  return result;
}

static void append_name_byte(char *name, size_t *at, uint8_t byte) {
  if (byte < 0x20 || byte >= 0x7f) {
    (void)snprintf(name + *at, 5, "\\x%02X", byte);
    *at += 4;
  } else {
    name[(*at)++] = (char)byte;
  }
}

static int format_short_name(const uint8_t *raw, char name[53]) {
  size_t base = 8, ext = 3, at = 0;
  while (base > 0 && raw[base - 1] == ' ')
    --base;
  while (ext > 0 && raw[8 + ext - 1] == ' ')
    --ext;
  for (size_t i = 0; i < base; ++i) {
    uint8_t c = raw[i];
    if (i == 0 && c == 0x05)
      c = 0xe5;
    append_name_byte(name, &at, c);
  }
  if (ext != 0) {
    name[at++] = '.';
    for (size_t i = 0; i < ext; ++i) {
      uint8_t c = raw[8 + i];
      append_name_byte(name, &at, c);
    }
  }
  name[at] = '\0';
  return 0;
}

static int parse_short_name(const char *input, uint8_t raw[11]) {
  size_t base = 0, ext = 0;
  bool after_dot = false;
  memset(raw, ' ', 11);
  if (*input == '\0')
    return -1;
  for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
    unsigned char c = *p;
    if (c == '.') {
      if (after_dot || base == 0)
        return -1;
      after_dot = true;
      continue;
    }
    if (c >= 0x80 || !(isalnum(c) || strchr("_$~!#%&'()-@^`{}", c) != NULL))
      return -1;
    c = (unsigned char)toupper(c);
    if (!after_dot) {
      if (base >= 8)
        return -1;
      raw[base++] = c;
    } else {
      if (ext >= 3)
        return -1;
      raw[8 + ext++] = c;
    }
  }
  return base == 0 ? -1 : 0;
}

static bool short_name_matches(const uint8_t entry_name[11],
                               const uint8_t wanted[11]) {
  for (size_t i = 0; i < 11; ++i) {
    unsigned char a = entry_name[i], b = wanted[i];
    if (a >= 'a' && a <= 'z')
      a = (unsigned char)(a - 'a' + 'A');
    if (a != b)
      return false;
  }
  return true;
}

static void decode_entry(const uint8_t raw[32], uint64_t entry_offset,
                         FatEntry *entry) {
  format_short_name(raw, entry->name);
  entry->attributes = raw[11];
  entry->first_cluster = (((uint32_t)le16(raw + 20) << 16) |
                          le16(raw + 26)) & 0x0fffffffu;
  entry->size = le32(raw + 28);
  entry->entry_offset = entry_offset;
}

/* Scan a directory chain. wanted==NULL lists; otherwise stop at the match. */
static int scan_directory(FatFs *fs, uint32_t start, const char *wanted_name,
                          FatEntry *found, bool print_entries) {
  uint8_t wanted[11];
  uint32_t *chain = NULL;
  size_t chain_count = 0;
  uint8_t cluster_data[MAX_SECTOR_SIZE * 128];
  if (wanted_name && parse_short_name(wanted_name, wanted) < 0)
    return set_error(fs, "path component is not a supported 8.3 name");
  if (collect_chain(fs, start, &chain, &chain_count) < 0)
    return -1;
  for (size_t cluster_index = 0; cluster_index < chain_count; ++cluster_index) {
    uint64_t offset;
    if (cluster_offset(fs, chain[cluster_index], &offset) < 0 ||
        read_exact(fs, offset, cluster_data, fs->cluster_bytes) < 0) {
      free(chain);
      return -1;
    }
    for (uint32_t pos = 0; pos + 32 <= fs->cluster_bytes; pos += 32) {
      const uint8_t *raw = cluster_data + pos;
      FatEntry entry;
      if (raw[0] == 0) {
        free(chain);
        return wanted_name ? set_error(fs, "path not found") : 0;
      }
      if (raw[0] == 0xe5 || raw[11] == 0x0f || (raw[11] & 0x08) != 0)
        continue;
      decode_entry(raw, offset + pos, &entry);
      if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
        continue;
      if (wanted_name) {
        if (short_name_matches(raw, wanted)) {
          *found = entry;
          free(chain);
          return 0;
        }
      } else if (print_entries) {
        printf("%-12s  %s  %10" PRIu32 " bytes  cluster=%" PRIu32 "\n",
               entry.name, (entry.attributes & 0x10) ? "dir " : "file",
               entry.size, entry.first_cluster);
      }
    }
  }
  free(chain);
  return wanted_name ? set_error(fs, "path not found") : 0;
}

static int resolve_path(FatFs *fs, const char *path, FatEntry *entry,
                        bool *is_root) {
  char *copy, *save = NULL, *component;
  uint32_t directory = fs->root_cluster;
  bool trailing_slash = *path && path[strlen(path) - 1] == '/';
  *is_root = false;
  while (*path == '/')
    ++path;
  if (*path == '\0' || strcmp(path, ".") == 0) {
    memset(entry, 0, sizeof(*entry));
    entry->name[0] = '/';
    entry->name[1] = '\0';
    entry->attributes = 0x10;
    entry->first_cluster = fs->root_cluster;
    *is_root = true;
    return 0;
  }
  copy = strdup(path);
  if (!copy)
    return set_error(fs, "out of memory while parsing path");
  memset(entry, 0, sizeof(*entry));
  entry->name[0] = '/';
  entry->name[1] = '\0';
  entry->attributes = 0x10;
  entry->first_cluster = fs->root_cluster;
  *is_root = true;
  component = strtok_r(copy, "/", &save);
  while (component) {
    FatEntry current;
    char *next_component = strtok_r(NULL, "/", &save);
    if (strcmp(component, ".") == 0) {
      component = next_component;
      continue;
    }
    if (strcmp(component, "..") == 0) {
      free(copy);
      return set_error(fs, "parent-directory path component is unsupported");
    }
    if (scan_directory(fs, directory, component, &current, false) < 0) {
      free(copy);
      return -1;
    }
    *entry = current;
    *is_root = false;
    if (next_component) {
      if ((current.attributes & 0x10) == 0) {
        free(copy);
        return set_error(fs, "a path component is not a directory");
      }
      if (current.first_cluster < 2 || current.first_cluster > fs->max_cluster) {
        free(copy);
        return set_error(fs, "directory entry has an invalid first cluster");
      }
      directory = current.first_cluster;
    } else {
      *entry = current;
    }
    component = next_component;
  }
  free(copy);
  if (trailing_slash && (entry->attributes & 0x10) == 0)
    return set_error(fs, "trailing slash requires a directory");
  return 0;
}

static int list_path(FatFs *fs, const char *path) {
  FatEntry entry;
  bool is_root;
  uint32_t cluster;
  if (resolve_path(fs, path, &entry, &is_root) < 0)
    return -1;
  if (!is_root && (entry.attributes & 0x10) == 0)
    return set_error(fs, "ls target is not a directory");
  cluster = is_root ? fs->root_cluster : entry.first_cluster;
  if (cluster < 2 || cluster > fs->max_cluster)
    return set_error(fs, "directory has an invalid first cluster");
  return scan_directory(fs, cluster, NULL, NULL, true);
}

static int print_stat(FatFs *fs, const char *path) {
  FatEntry entry;
  bool is_root;
  if (resolve_path(fs, path, &entry, &is_root) < 0)
    return -1;
  printf("Name: %s\nType: %s\nAttributes: 0x%02x\nFirst cluster: %" PRIu32
         "\nSize: %" PRIu32 " bytes\nDirectory entry offset: %" PRIu64 "\n",
         entry.name, (entry.attributes & 0x10) ? "directory" : "regular file",
         entry.attributes, entry.first_cluster, entry.size, entry.entry_offset);
  if (is_root)
    printf("Note: FAT32 root directory has no fixed entry or file size.\n");
  return 0;
}

static int stream_file_range(FatFs *fs, const FatEntry *entry, uint64_t offset,
                             uint64_t length, ChunkFn consume, void *context) {
  uint64_t available, remaining, skip, intra, last_cluster_index;
  uint64_t capacity = (uint64_t)fs->cluster_count * fs->cluster_bytes;
  uint32_t *chain = NULL;
  size_t chain_count = 0;
  uint8_t block[MAX_SECTOR_SIZE * 128];
  if ((entry->attributes & 0x10) != 0)
    return set_error(fs, "cannot read a directory as a regular file");
  if ((uint64_t)entry->size > capacity)
    return set_error(fs, "file size exceeds the volume's data capacity");
  if (offset > entry->size)
    return set_error(fs, "read offset is beyond end-of-file");
  available = (uint64_t)entry->size - offset;
  remaining = length < available ? length : available;
  if (remaining == 0)
    return 0;
  if (entry->first_cluster < 2 || entry->first_cluster > fs->max_cluster)
    return set_error(fs, "non-empty file has an invalid first cluster");
  if (collect_chain(fs, entry->first_cluster, &chain, &chain_count) < 0)
    return -1;
  skip = offset / fs->cluster_bytes;
  intra = offset % fs->cluster_bytes;
  last_cluster_index = skip + (intra + remaining - 1) / fs->cluster_bytes;
  if (((uint64_t)entry->size + fs->cluster_bytes - 1) / fs->cluster_bytes > chain_count) {
    free(chain);
    return set_error(fs, "file cluster chain ended before the requested bytes");
  }
  for (uint64_t index = skip; index <= last_cluster_index; ++index) {
      uint64_t start = index == skip ? intra : 0;
      uint64_t chunk = fs->cluster_bytes - start;
      uint64_t cluster_pos;
      if (chunk > remaining)
        chunk = remaining;
      if (cluster_offset(fs, chain[index], &cluster_pos) < 0 ||
          read_exact(fs, cluster_pos + start, block, (size_t)chunk) < 0) {
        free(chain);
        return -1;
      }
      if (consume(block, (size_t)chunk, context) != 0) {
        free(chain);
        return set_error(fs, "output callback failed");
      }
      remaining -= chunk;
  }
  free(chain);
  return 0;
}

static int output_raw(const uint8_t *bytes, size_t length, void *context) {
  FILE *out = context;
  return fwrite(bytes, 1, length, out) == length ? 0 : -1;
}

typedef struct { bool first; } HexOutput;

static int output_hex(const uint8_t *bytes, size_t length, void *context) {
  HexOutput *state = context;
  for (size_t i = 0; i < length; ++i) {
    printf("%s%02x", state->first ? "" : " ", bytes[i]);
    state->first = false;
  }
  return 0;
}

static int print_info(FatFs *fs) {
  printf("FAT type: FAT32\nBytes per sector: %" PRIu32
         "\nSectors per cluster: %" PRIu32 "\nCluster size: %" PRIu32
         " bytes\nReserved sectors: %" PRIu32 "\nFAT count: %" PRIu32
         "\nSectors per FAT: %" PRIu32 "\nTotal sectors: %" PRIu32
         "\nData clusters: %" PRIu32 "\nRoot cluster: %" PRIu32
         "\nVolume bytes: %" PRIu64 "\n",
         fs->bytes_per_sector, fs->sectors_per_cluster, fs->cluster_bytes,
         fs->reserved_sectors, fs->fat_count, fs->fat_sectors,
         fs->total_sectors, fs->cluster_count, fs->root_cluster,
         (uint64_t)fs->total_sectors * fs->bytes_per_sector);
  return 0;
}

static void usage(const char *program) {
  fprintf(stderr,
          "Usage:\n"
          "  %s IMAGE info\n"
          "  %s IMAGE ls [PATH]\n"
          "  %s IMAGE stat PATH\n"
          "  %s IMAGE cat PATH\n"
          "  %s IMAGE read PATH OFFSET LENGTH\n"
          "Paths use FAT 8.3 names; '/' selects the volume root.\n",
          program, program, program, program, program);
}

int main(int argc, char **argv) {
  FatFs fs;
  const char *image, *command, *path;
  int result = -1;
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  image = argv[1];
  command = argv[2];
  if (fat_open(&fs, image) < 0) {
    fprintf(stderr, "fat32: %s\n", fs.error);
    fat_close(&fs);
    return 1;
  }
  if (strcmp(command, "info") == 0 && argc == 3) {
    result = print_info(&fs);
  } else if (strcmp(command, "ls") == 0 && (argc == 3 || argc == 4)) {
    result = list_path(&fs, argc == 4 ? argv[3] : "/");
  } else if (strcmp(command, "stat") == 0 && argc == 4) {
    result = print_stat(&fs, argv[3]);
  } else if ((strcmp(command, "cat") == 0 || strcmp(command, "read") == 0) &&
             ((strcmp(command, "cat") == 0 && argc == 4) ||
              (strcmp(command, "read") == 0 && argc == 6))) {
    FatEntry entry;
    bool is_root;
    path = argv[3];
    if (resolve_path(&fs, path, &entry, &is_root) < 0) {
      result = -1;
    } else if (strcmp(command, "cat") == 0) {
      result = stream_file_range(&fs, &entry, 0, entry.size, output_raw, stdout);
    } else {
      char *end_offset, *end_length;
      errno = 0;
      uint64_t offset = strtoull(argv[4], &end_offset, 10);
      uint64_t length = strtoull(argv[5], &end_length, 10);
      HexOutput state = { .first = true };
      if (errno || *argv[4] == '\0' || *end_offset != '\0' ||
          *argv[5] == '\0' || *end_length != '\0' ||
          strspn(argv[4], "0123456789") != strlen(argv[4]) ||
          strspn(argv[5], "0123456789") != strlen(argv[5])) {
        result = set_error(&fs, "read offset and length must be unsigned decimal integers");
      } else {
        result = stream_file_range(&fs, &entry, offset, length, output_hex, &state);
        if (result == 0)
          putchar('\n');
      }
    }
  } else {
    usage(argv[0]);
    fat_close(&fs);
    return 2;
  }
  if (result == 0 && fflush(stdout) != 0)
    result = set_error(&fs, "failed to flush output");
  if (result < 0)
    fprintf(stderr, "fat32: %s\n", fs.error);
  fat_close(&fs);
  return result < 0 ? 1 : 0;
}
