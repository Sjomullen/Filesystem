#include "fs.h"
#include "disk.h"
#include <stdint.h>
//Using C++20


FS::FS()
{

    // Ensure disk is initialized
    if (!disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat))) {
        std::cerr << "Warning: Unable to read FAT; initializing as new disk" << std::endl;
    }
}

FS::~FS()
{

}

// formats the disk, i.e., creates an empty file system
int FS::format() {

    // Initialize FAT (mark all blocks as free except for block 0 and block 1)
    for (unsigned i = 0; i < BLOCK_SIZE / 2; ++i) {
        if (i == ROOT_BLOCK || i == FAT_BLOCK) {
            fat[i] = FAT_EOF; // Mark root and FAT blocks as special
        } else {
            fat[i] = FAT_FREE; // Mark other blocks as free
        }
    }

    // Initialize root directory
    dir_entry root_dir;
    std::memset(&root_dir, 0, sizeof(root_dir));
    root_dir.first_blk = ROOT_BLOCK;  // Root directory's first block
    root_dir.type = TYPE_DIR;         // Root is a directory
    root_dir.size = 0;                // Initially empty

    // Write root directory entry to block 0 (the root block)
    uint8_t root_block[BLOCK_SIZE] = {0};
    std::memcpy(root_block, &root_dir, sizeof(root_dir));
    disk.write(ROOT_BLOCK, root_block);

    std::cout << "Disk formatted successfully\n";
    return 0;
}

int FS::create(std::string filepath) {

  	// Validate file name length
    if (filepath.length() >= 56) {
        std::cerr << "Error: File name exceeds the maximum length of 55 characters.\n";
        return -1; // Error code for invalid file name
    }
    // Check if the file already exists by searching the root directory
    uint8_t root_block[BLOCK_SIZE];
    disk.read(ROOT_BLOCK, root_block);
    dir_entry* root_entries = reinterpret_cast<dir_entry*>(root_block);

    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name == filepath) {
            std::cout << "File already exists: " << filepath << "\n";
            return -1; // File exists, return error
        }
    }
    // Declare and initialize entry_count
    int entry_count = 0;

    // Iterate through all directory entries
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name[0] != '\0') { // Entry is in use
            entry_count++;
            if (std::string(root_entries[i].file_name) == filepath) {
                std::cerr << "Error: File already exists: " << filepath << "\n";
                return -1; // File already exists
            }
        }
    }
	    // Add this check for directory full condition
    if (entry_count >= BLOCK_SIZE / sizeof(dir_entry)) {
        std::cerr << "Error: Directory full, cannot add more entries.\n";
        return -1; // Return error if the directory is full
    }

    // Find free block for the file
    int first_free_block = -1;
    for (int i = 0; i < BLOCK_SIZE / 2; ++i) {
        if (fat[i] == FAT_FREE) {
            first_free_block = i;
            break;
        }
    }

    if (first_free_block == -1) {
        std::cout << "No free block available\n";
        return -1; // No free blocks
    }

    // Write data (accept multiple lines from user until empty line)
    std::string file_data;
    std::string line;
    while (std::getline(std::cin, line) && !line.empty()) {
        file_data += line + "\n";
    }

    // Split file data into blocks
    size_t total_blocks = (file_data.size() + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int last_block = first_free_block;
    for (size_t i = 0; i < total_blocks; ++i) {
        uint8_t block[BLOCK_SIZE] = {0};
        size_t start = i * BLOCK_SIZE;
        size_t len = std::min(static_cast<size_t>(BLOCK_SIZE), file_data.size() - start);
        std::memcpy(block, file_data.data() + start, len);
        disk.write(last_block, block);

        // Update FAT
        if (i < total_blocks - 1) {
            fat[last_block] = i + 1;
            last_block = i + 1;
        } else {
            fat[last_block] = FAT_EOF; // Mark last block as EOF
        }
    }

    // Write FAT back to disk
    uint8_t fat_block[BLOCK_SIZE] = {0};
    std::memcpy(fat_block, fat, sizeof(fat));
    disk.write(FAT_BLOCK, fat_block);

    // Update root directory with the new file
    dir_entry new_file = {0};
    std::strncpy(new_file.file_name, filepath.c_str(), sizeof(new_file.file_name) - 1);
    new_file.first_blk = first_free_block;
    new_file.size = file_data.size();
    new_file.type = TYPE_FILE;
    new_file.access_rights = READ | WRITE;

    disk.read(ROOT_BLOCK, root_block);
    root_entries = reinterpret_cast<dir_entry*>(root_block);

    // Find the first free entry in the root directory
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name[0] == '\0') {
            root_entries[i] = new_file;
            break;
        }
    }

    // Write updated root directory back to disk
    disk.write(ROOT_BLOCK, root_block);

    std::cout << "File " << filepath << " created successfully\n";
    return 0;
}

int FS::cat(std::string filepath) {

    // Find file in root directory
    uint8_t root_block[BLOCK_SIZE];
    disk.read(ROOT_BLOCK, root_block);
    dir_entry* root_entries = reinterpret_cast<dir_entry*>(root_block);

    dir_entry* file_entry = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name == filepath) {
            file_entry = &root_entries[i];
            break;
        }
    }

    if (!file_entry) {
        std::cout << "File not found: " << filepath << "\n";
        return -1; // File not found
    }

    // Read the file data using the FAT
    int block = file_entry->first_blk;
    while (block != FAT_EOF) {
        uint8_t block_data[BLOCK_SIZE] = {0};
        disk.read(block, block_data);
        std::cout.write(reinterpret_cast<char*>(block_data), BLOCK_SIZE);
        block = fat[block];
    }

    return 0;
}


// ls lists the content in the currect directory (files and sub-directories)
int FS::ls() {
    uint8_t root_block[BLOCK_SIZE];
    disk.read(ROOT_BLOCK, root_block);
    dir_entry* root_entries = reinterpret_cast<dir_entry*>(root_block);

    // Print header line
    std::cout << "name\tsize\n";

    // Iterate through root directory entries and print files
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (dir_entries[i].file_name[0] != '\0') {  // Check if the entry is not empty
            std::cout << dir_entries[i].file_name << "\t";

            // Print the type: 'dir' for directory, 'file' for file
            if (dir_entries[i].type == 1) {
                std::cout << "dir";
            } else {
                std::cout << "file";
            }

            // Print the size
            std::cout << "\t" << dir_entries[i].size << "\n";
        }
    }

    return 0;
}


// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int FS::cp(std::string sourcepath, std::string destpath) {

    // Locate source file in root directory
    uint8_t root_block[BLOCK_SIZE];
    disk.read(ROOT_BLOCK, root_block);
    dir_entry* root_entries = reinterpret_cast<dir_entry*>(root_block);

    dir_entry* source_file = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name == sourcepath) {
            source_file = &root_entries[i];
            break;
        }
    }

    if (!source_file) {
        std::cout << "Source file not found: " << sourcepath << "\n";
        return -1; // Source file not found
    }

    // Check if destination file already exists
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name == destpath) {
            std::cout << "Destination file already exists: " << destpath << "\n";
            return -1; // Destination file exists
        }
    }

    // Allocate a new directory entry for the destination file
    dir_entry new_file = {0};
    std::strncpy(new_file.file_name, destpath.c_str(), sizeof(new_file.file_name) - 1);
    new_file.size = source_file->size;
    new_file.type = source_file->type;
    new_file.access_rights = source_file->access_rights;

    // Copy the file's content block by block
    int src_block = source_file->first_blk;
    int prev_block = -1;

    while (src_block != FAT_EOF) {
        // Find a free block for the copy
        int dest_block = -1;
        for (int i = 0; i < BLOCK_SIZE / 2; ++i) {
            if (fat[i] == FAT_FREE) {
                dest_block = i;
                break;
            }
        }
        if (dest_block == -1) {
            std::cout << "No free blocks available for copying\n";
            return -1;
        }

        // Read source block and write to destination block
        uint8_t block_data[BLOCK_SIZE] = {0};
        disk.read(src_block, block_data);
        disk.write(dest_block, block_data);

        // Update FAT for the new file
        if (prev_block == -1) {
            new_file.first_blk = dest_block; // First block of the new file
        } else {
            fat[prev_block] = dest_block; // Link the previous block
        }
        prev_block = dest_block;

        // Continue to the next block in the source file
        src_block = fat[src_block];
    }

    // Mark the last block as EOF
    fat[prev_block] = FAT_EOF;

    // Write FAT back to disk
    uint8_t fat_block[BLOCK_SIZE] = {0};
    std::memcpy(fat_block, fat, sizeof(fat));
    disk.write(FAT_BLOCK, fat_block);

    // Add the new file to the root directory
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name[0] == '\0') {
            root_entries[i] = new_file;
            break;
        }
    }

    // Write updated root directory back to disk
    disk.write(ROOT_BLOCK, root_block);

    std::cout << "File copied successfully\n";
    return 0;
}


// mv <sourcepath> <destpath> renames the file <sourcepath> to the name <destpath>,
// or moves the file <sourcepath> to the directory <destpath> (if dest is a directory)
int FS::mv(std::string sourcepath, std::string destpath) {

    // Locate source file in root directory
    uint8_t root_block[BLOCK_SIZE];
    disk.read(ROOT_BLOCK, root_block);
    dir_entry* root_entries = reinterpret_cast<dir_entry*>(root_block);

    dir_entry* source_file = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name == sourcepath) {
            source_file = &root_entries[i];
            break;
        }
    }

    if (!source_file) {
        std::cout << "Source file not found: " << sourcepath << "\n";
        return -1; // Source file not found
    }

    // Check if destination file already exists
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name == destpath) {
            std::cout << "Destination file already exists: " << destpath << "\n";
            return -1; // Destination file exists
        }
    }

    // Rename the file
    std::strncpy(source_file->file_name, destpath.c_str(), sizeof(source_file->file_name) - 1);

    // Write updated root directory back to disk
    disk.write(ROOT_BLOCK, root_block);

    std::cout << "File renamed successfully\n";
    return 0;
}


// rm <filepath> removes / deletes the file <filepath>
int FS::rm(std::string filepath) {

    // Locate the file in root directory
    uint8_t root_block[BLOCK_SIZE];
    disk.read(ROOT_BLOCK, root_block);
    dir_entry* root_entries = reinterpret_cast<dir_entry*>(root_block);

    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (root_entries[i].file_name == filepath) {
            // Free all blocks used by the file
            int block = root_entries[i].first_blk;
            while (block != FAT_EOF) {
                int next_block = fat[block];
                fat[block] = FAT_FREE;
                block = next_block;
            }

            // Clear the directory entry
            std::memset(&root_entries[i], 0, sizeof(dir_entry));

            // Write FAT and root directory back to disk
            uint8_t fat_block[BLOCK_SIZE] = {0};
            std::memcpy(fat_block, fat, sizeof(fat));
            disk.write(FAT_BLOCK, fat_block);
            disk.write(ROOT_BLOCK, root_block);

            std::cout << "File deleted successfully\n";
            return 0;
        }
    }

    std::cout << "File not found: " << filepath << "\n";
    return -1; // File not found
}


// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int FS::append(std::string filepath1, std::string filepath2) {
    // Buffer to read/write disk blocks
    uint8_t buffer[BLOCK_SIZE];

    // Locate file1 and file2 in the directory
    dir_entry file1, file2;
    bool file1_found = false, file2_found = false;
    disk.read(ROOT_BLOCK, buffer);
    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(buffer);

    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (std::string(dir_entries[i].file_name) == filepath1) {
            file1 = dir_entries[i];
            file1_found = true;
        }
        if (std::string(dir_entries[i].file_name) == filepath2) {
            file2 = dir_entries[i];
            file2_found = true;
        }
    }

    if (!file1_found || !file2_found) {
        std::cerr << "Error: One or both files do not exist.\n";
        return -1;
    }

    // Check access rights
    if (!(file1.access_rights & READ)) {
        std::cerr << "Error: No read access to " << filepath1 << "\n";
        return -1;
    }
    if (!(file2.access_rights & WRITE)) {
        std::cerr << "Error: No write access to " << filepath2 << "\n";
        return -1;
    }

    // Traverse FAT and read content of file1
    std::vector<uint8_t> file1_content;
    int16_t current_block = file1.first_blk;

    while (current_block != FAT_EOF) {
        disk.read(current_block, buffer);
        if (current_block == file1.first_blk) {
            file1_content.insert(file1_content.end(), buffer, buffer + file1.size);
        } else {
            file1_content.insert(file1_content.end(), buffer, buffer + BLOCK_SIZE);
        }
        current_block = fat[current_block];
    }

    // Traverse FAT to find the last block of file2
    current_block = file2.first_blk;
    int16_t last_block = -1;
    int space_in_last_block = 0;

    while (current_block != FAT_EOF) {
        last_block = current_block;
        current_block = fat[current_block];
    }

    if (last_block != -1) {
        // Check if the last block of file2 has space
        space_in_last_block = BLOCK_SIZE - (file2.size % BLOCK_SIZE);
    }

    // Write the content of file1 to file2
    size_t remaining_data = file1_content.size();
    size_t data_offset = 0;

    if (space_in_last_block > 0 && last_block != -1) {
        // Write to the last block if there's space
        disk.read(last_block, buffer);
        size_t write_size = std::min(static_cast<size_t>(space_in_last_block), remaining_data);
        std::memcpy(buffer + (BLOCK_SIZE - space_in_last_block), file1_content.data(), write_size);
        disk.write(last_block, buffer);
        remaining_data -= write_size;
        data_offset += write_size;
        file2.size += write_size;
    }

    // Allocate new blocks for remaining data
    while (remaining_data > 0) {
        // Find a free block in FAT
        int16_t new_block = -1;
        for (int i = 0; i < BLOCK_SIZE / 2; ++i) {
            if (fat[i] == FAT_FREE) {
                new_block = i;
                fat[i] = FAT_EOF;
                break;
            }
        }

        if (new_block == -1) {
            std::cerr << "Error: No free blocks available.\n";
            return -1;
        }

        if (last_block != -1) {
            fat[last_block] = new_block;
        } else {
            file2.first_blk = new_block;
        }

        last_block = new_block;

        size_t write_size = std::min(static_cast<size_t>(BLOCK_SIZE), remaining_data);
        std::memcpy(buffer, file1_content.data() + data_offset, write_size);
        disk.write(new_block, buffer);
        remaining_data -= write_size;
        data_offset += write_size;
        file2.size += write_size;
    }

    // Update the FAT and directory entries on disk
    disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat));

    // Update file2 directory entry with the new size and first block (if modified)
    disk.read(ROOT_BLOCK, buffer);  // Re-read root block before modifying
    dir_entries = reinterpret_cast<dir_entry*>(buffer); // Update dir_entries pointer after reading
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (std::string(dir_entries[i].file_name) == filepath2) {
            dir_entries[i] = file2;  // Update file2 entry
            break;
        }
    }

    disk.write(ROOT_BLOCK, buffer);  // Write back updated directory

    return 0;
}



// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int FS::mkdir(std::string dirpath) {
    uint8_t buffer[BLOCK_SIZE];
    disk.read(ROOT_BLOCK, buffer);  // Read root directory block
    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(buffer);

    // Find an empty entry for the new directory
    int empty_index = -1;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i) {
        if (dir_entries[i].file_name[0] == '\0') {  // Check for an empty slot
            empty_index = i;
            break;
        }
    }

    if (empty_index == -1) {
        std::cerr << "Error: No empty directory entry available.\n";
        return -1;  // No space for new directory
    }

    // Create the new directory entry
    dir_entry new_dir;
    std::strncpy(new_dir.file_name, dirpath.c_str(), sizeof(new_dir.file_name) - 1);  // Set the directory name
    new_dir.size = 0;  // Initially, the directory is empty
    new_dir.first_blk = FAT_FREE;  // We'll allocate the first block shortly
    new_dir.type = 1;  // This is a directory
    new_dir.access_rights = 0;  // Default access rights (you can modify as needed)

    // Find the first free block in the FAT for the new directory's first block
    int new_block = -1;
    for (int i = 0; i < BLOCK_SIZE / 2; ++i) {
        if (fat[i] == FAT_FREE) {
            new_block = i;
            fat[i] = FAT_EOF;  // Mark this block as the end of the directory
            break;
        }
    }

    if (new_block == -1) {
        std::cerr << "Error: No free blocks available for the new directory.\n";
        return -1;  // No space for directory blocks
    }

    // Update the new directory's first block
    new_dir.first_blk = new_block;

    // Create the special ".." entry for the parent directory inside the new directory
    uint8_t dir_block[BLOCK_SIZE] = {0};  // Clear the block
    dir_entry* dir_block_entries = reinterpret_cast<dir_entry*>(dir_block);

    // Add the parent directory entry
    dir_entry parent_entry;
    std::strncpy(parent_entry.file_name, "..", sizeof(parent_entry.file_name) - 1);
    parent_entry.size = 0;
    parent_entry.first_blk = ROOT_BLOCK;  // This is the parent (the root directory in this case)
    parent_entry.type = 1;  // This is a directory (the parent is a directory)
    parent_entry.access_rights = 0;

    dir_block_entries[0] = parent_entry;  // Store the parent directory entry

    // Write the new directory block to disk
    disk.write(new_block, dir_block);

    // Insert the new directory into the root directory's entries
    dir_entries[empty_index] = new_dir;

    // Write the updated root directory block to disk
    disk.write(ROOT_BLOCK, reinterpret_cast<uint8_t*>(dir_entries));

    std::cout << "Directory " << dirpath << " created successfully.\n";
    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named <dirpath>
int FS::cd(std::string dirpath) {
    std::cout << "FS::cd(" << dirpath << ")\n";

    if (dirpath == "/") {
        // Go to root directory
        current_dir = ROOT_BLOCK;
        return 0;
    }

    // Handle relative paths and parent directory
    if (dirpath == "..") {
        if (current_dir == ROOT_BLOCK) {
            std::cout << "Error: Already at root directory\n";
            return -1;
        }
        // Fetch and set the parent directory
        dir_entry parent_entry;
        disk.read(current_dir, reinterpret_cast<uint8_t*>(&parent_entry));
        current_dir = parent_entry.first_blk;
        return 0;
    }

    // Handle navigation to a specific sub-directory
    dir_entry entries[BLOCK_SIZE / sizeof(dir_entry)];
    disk.read(current_dir, reinterpret_cast<uint8_t*>(entries));

    for (auto &entry : entries) {
        if (entry.type == TYPE_DIR && dirpath == std::string(entry.file_name)) {
            current_dir = entry.first_blk;
            return 0;
        }
    }

    std::cout << "Error: Directory not found\n";
    return -1;
}


// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the currect directory name
int FS::pwd()
{
    return 0;
}

// chmod <accessrights> <filepath> changes the access rights for the
// file <filepath> to <accessrights>.
int FS::chmod(std::string accessrights, std::string filepath)
{
    return 0;
}
