// fs.h
#ifndef __FS_H__
#define __FS_H__

#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "disk.h"

#define ROOT_BLOCK 0
#define FAT_BLOCK 1
#define FAT_FREE 0
#define FAT_EOF -1

#define TYPE_FILE 0
#define TYPE_DIR 1
#define READ 0x04
#define WRITE 0x02
#define EXECUTE 0x01

struct dir_entry {
    char    file_name[56];       // name of the file / sub-directory
    uint32_t size;               // size of the file in bytes
    uint16_t first_blk;          // index in the FAT for the first block of the file
    uint8_t  type;               // directory (1) or file (0)
    uint8_t  access_rights;      // read (0x04), write (0x02), execute (0x01)
};

constexpr size_t MAX_NAME_LEN = sizeof(dir_entry::file_name) - 1;

class FS {
private:
    Disk disk;
    int16_t fat[BLOCK_SIZE/2];           // in-memory FAT
    uint16_t current_dir = ROOT_BLOCK;   // block number of current directory

    // Helper: write raw data across chained blocks
    int write_to_file(const std::string &filepath,
                      const uint8_t *data,
                      size_t size);

    // Resolve an absolute or relative path into:
    //   out_dir  = block number of containing directory
    //   out_name = final component (file or directory name)
    // Returns 0 on success, -1 on failure.
    int resolve_path(const std::string &path,
                     uint16_t &out_dir,
                     std::string &out_name);

    int find_free_block();

public:
    FS();
    ~FS();

    int format();
    int create(std::string filepath);
    int cat(std::string filepath);
    int ls();

    int cp(std::string sourcepath, std::string destpath);
    int mv(std::string sourcepath, std::string destpath);
    int rm(std::string filepath);
    int append(std::string filepath1, std::string filepath2);

    int mkdir(std::string dirpath);
    int cd(std::string dirpath);
    int pwd();
    int chmod(std::string accessrights, std::string filepath);

    bool is_directory(uint16_t dir_block);
    uint16_t get_parent_directory(uint16_t dir_block);
};

#endif