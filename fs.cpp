// fs.cpp
#include "fs.h"
#include <algorithm>
#include <sstream>
#include <vector>
#include <cstring>
#include <iostream>

// Constructor: load on‐disk FAT or format fresh
FS::FS()
  : current_dir(ROOT_BLOCK)
{
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0) {
        format();
    }
}

FS::~FS() { }

// Format the disk: initialize FAT and clear root directory
int FS::format() {
    // mark block 0 and 1 as EOF, others free
    for (int i = 0; i < BLOCK_SIZE/2; ++i) {
        fat[i] = (i == ROOT_BLOCK || i == FAT_BLOCK) ? FAT_EOF : FAT_FREE;
    }
    // write FAT
    {
        uint8_t buf[BLOCK_SIZE] = {0};
        std::memcpy(buf, fat, sizeof(fat));
        if (disk.write(FAT_BLOCK, buf) != 0) return -1;
    }
    // clear root dir block
    {
        uint8_t buf[BLOCK_SIZE] = {0};
        if (disk.write(ROOT_BLOCK, buf) != 0) return -1;
    }
    current_dir = ROOT_BLOCK;
    return 0;
}

// Helper: split pathname into directory block + final name
int FS::resolve_path(const std::string &path,
                     uint16_t &out_dir,
                     std::string &out_name)
{
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string tok;
    while (std::getline(ss, tok, '/')) parts.push_back(tok);

    // start at ROOT if leading '/', else at current_dir
    uint16_t dir = (!path.empty() && path[0]=='/') ? ROOT_BLOCK : current_dir;
    size_t start = (!path.empty() && path[0]=='/') ? 1 : 0;

    for (size_t i = start; i + 1 < parts.size(); ++i) {
        const auto &c = parts[i];
        if (c.empty() || c == ".") continue;
        if (c == "..") {
            dir = get_parent_directory(dir);
            continue;
        }
        // find subdir c in dir
        uint8_t buf[BLOCK_SIZE];
        disk.read(dir, buf);
        auto *ents = reinterpret_cast<dir_entry*>(buf);
        bool found = false;
        int slots = BLOCK_SIZE / sizeof(dir_entry);
        for (int j = 0; j < slots; ++j) {
            if (c == ents[j].file_name && ents[j].type == TYPE_DIR) {
                dir = ents[j].first_blk;
                found = true;
                break;
            }
        }
        if (!found) return -1;
    }

    out_dir  = dir;
    out_name = parts.empty() ? "" : parts.back();
    return 0;
}

// Helper: write data across FAT‐chained blocks; return first block index
int FS::write_to_file(const std::string & /*unused*/,
                      const uint8_t *data,
                      size_t size)
{
    int first = -1;
    for (int i = 2; i < BLOCK_SIZE/2; ++i) {
        if (fat[i] == FAT_FREE) { first = i; break; }
    }
    if (first < 0) return -1;

    int prev = first;
    size_t written = 0;
    uint8_t buf[BLOCK_SIZE];

    while (written < size) {
        size_t chunk = std::min<size_t>(BLOCK_SIZE, size - written);
        std::memset(buf, 0, BLOCK_SIZE);
        std::memcpy(buf, data + written, chunk);
        disk.write(prev, buf);
        written += chunk;

        if (written < size) {
            int nxt = -1;
            for (int i = 2; i < BLOCK_SIZE/2; ++i) {
                if (fat[i] == FAT_FREE) { nxt = i; break; }
            }
            if (nxt < 0) return -1;
            fat[prev] = nxt;
            prev = nxt;
        } else {
            fat[prev] = FAT_EOF;
        }
    }

    // persist FAT
    {
        uint8_t fbuf[BLOCK_SIZE] = {0};
        std::memcpy(fbuf, fat, sizeof(fat));
        disk.write(FAT_BLOCK, fbuf);
    }
    return first;
}

// create: make or overwrite file from stdin until blank line
int FS::create(std::string filepath) {
    uint16_t dirblk;
    std::string name;
    if (resolve_path(filepath, dirblk, name) != 0 || name.empty())
        return -1;

    if (name.length() > MAX_NAME_LEN) {
        std::cout << "Error: File name too long (max 55 characters allowed)\n";
        return -1;
    }

    // load directory
    uint8_t dirbuf[BLOCK_SIZE];
    disk.read(dirblk, dirbuf);
    auto *ents = reinterpret_cast<dir_entry*>(dirbuf);
    int slots = BLOCK_SIZE / sizeof(dir_entry);

    // check duplicate & fullness
    int used = 0;
    for (int i = 0; i < slots; ++i) {
        if (ents[i].file_name[0]) {
            ++used;
            if (name == ents[i].file_name) return -1;
        }
    }
    if (used >= slots) return -1;

    // read data
    std::string data, line;
    while (std::getline(std::cin, line) && !line.empty())
        data += line + "\n";

    // write blocks
    int first = write_to_file(name,
        reinterpret_cast<const uint8_t*>(data.data()),
        data.size());
    if (first < 0) return -1;

    // new entry
    dir_entry nde = {};
    std::strncpy(nde.file_name, name.c_str(), MAX_NAME_LEN);
    nde.file_name[MAX_NAME_LEN] = '\0';
    nde.size          = data.size();
    nde.first_blk     = first;
    nde.type          = TYPE_FILE;
    nde.access_rights = READ | WRITE;

    // insert
    for (int i = 0; i < slots; ++i) {
        if (!ents[i].file_name[0]) {
            ents[i] = nde;
            break;
        }
    }
    disk.write(dirblk, dirbuf);
    return 0;
}

// cat: print file contents
int FS::cat(std::string filepath) {
    uint16_t dirblk;
    std::string name;
    if (resolve_path(filepath, dirblk, name) != 0 || name.empty()) {
        std::cout << "Error: File not found: " << filepath << std::endl;
        return -1;
    }

    uint8_t dirbuf[BLOCK_SIZE];
    disk.read(dirblk, dirbuf);
    auto *ents = reinterpret_cast<dir_entry*>(dirbuf);
    int slots = BLOCK_SIZE / sizeof(dir_entry);

    dir_entry *fe = nullptr;
    for (int i = 0; i < slots; ++i) {
        if (name == ents[i].file_name) {
            fe = &ents[i];
            break;
        }
    }
    if (!fe) {
        std::cout << "Error: File not found: " << filepath << std::endl;
        return -1;
    }
    if (fe->type != TYPE_FILE) {
        std::cout << "Error: " << filepath << " is a directory" << std::endl;
        return -1;
    }
    if (!(fe->access_rights & READ)) {
        std::cout << "Error: Permission denied (no read access) on " << filepath << std::endl;
        return -1;
    }

    size_t rem = fe->size;
    int blk = fe->first_blk;
    uint8_t buf[BLOCK_SIZE];
    while (blk != FAT_EOF && rem > 0) {
        disk.read(blk, buf);
        size_t to_write = std::min<size_t>(BLOCK_SIZE, rem);
        std::cout.write(reinterpret_cast<char*>(buf), to_write);
        rem -= to_write;
        blk = fat[blk];
    }
    return 0;
}

// ls: list current_dir, sorted, with name, type, size
int FS::ls() {
    uint8_t buf[BLOCK_SIZE];
    disk.read(current_dir, buf);
    auto *ents = reinterpret_cast<dir_entry*>(buf);
    int slots = BLOCK_SIZE / sizeof(dir_entry);

    struct Entry { std::string name; bool is_dir; uint32_t size; uint8_t rights; };
    std::vector<Entry> all;
    for (int i = 0; i < slots; ++i) {
        if (ents[i].file_name[0] == 0) continue;
        all.push_back({
            ents[i].file_name,
            ents[i].type == TYPE_DIR,
            ents[i].size,
            ents[i].access_rights
        });
    }
    std::sort(all.begin(), all.end(),
              [](auto &a, auto &b){ return a.name < b.name; });

    // new header with accessrights
    std::cout << "name\t type\t accessrights\t size\n";
    for (auto &e : all) {
        // build "rwx" string
        std::string rights;
        rights += (e.rights & READ)    ? 'r' : '-';
        rights += (e.rights & WRITE)   ? 'w' : '-';
        rights += (e.rights & EXECUTE) ? 'x' : '-';

        std::cout
          << e.name << "\t"
          << (e.is_dir ? "dir" : "file") << "\t"
          << rights << "\t"
          << (e.is_dir ? "-" : std::to_string(e.size))
          << "\n";
    }
    return 0;
}

// cp: copy file or into directory
int FS::cp(std::string sourcepath, std::string destpath) {
    // resolve source
    uint16_t sdir; std::string sname;
    if (resolve_path(sourcepath, sdir, sname)!=0 || sname.empty())
        return -1;
    uint8_t sb[BLOCK_SIZE]; disk.read(sdir,sb);
    auto *sents = reinterpret_cast<dir_entry*>(sb);
    int slots = BLOCK_SIZE/sizeof(dir_entry);
    dir_entry *src = nullptr;
    for (int i = 0; i < slots; ++i)
        if (sname==sents[i].file_name && sents[i].type==TYPE_FILE)
            src=&sents[i];
    if (!src) return -1;

    // resolve dest
    uint16_t ddir; std::string dname;
    bool into_dir=false;
    if (resolve_path(destpath, ddir, dname)==0 && !dname.empty()) {
        uint8_t db[BLOCK_SIZE]; disk.read(ddir,db);
        auto *dents = reinterpret_cast<dir_entry*>(db);
        for (int i=0;i<slots;++i){
            if (dname==dents[i].file_name && dents[i].type==TYPE_DIR){
                into_dir=true;
                ddir = dents[i].first_blk;
                dname = sname;
                break;
            }
            if (dname==dents[i].file_name) return -1;
        }
    } else {
        ddir = current_dir;
        dname = destpath;
    }

        if (dname.length() > MAX_NAME_LEN) {
        std::cout << "Error: Destination name too long (max " << MAX_NAME_LEN << " characters)\n";
        return -1;
    }

    // read src data
    std::vector<uint8_t> data;
    {
        size_t rem = src->size;
        int16_t b = src->first_blk;
        uint8_t buf[BLOCK_SIZE];
        while(b!=FAT_EOF && rem>0){
            disk.read(b,buf);
            size_t c = std::min<size_t>(BLOCK_SIZE, rem);
            data.insert(data.end(), buf, buf+c);
            rem -= c; b = fat[b];
        }
    }

    int first = write_to_file(dname, data.data(), data.size());
    if (first<0) return -1;

    // insert into ddir
    uint8_t dbuf[BLOCK_SIZE]; disk.read(ddir,dbuf);
    auto *dents = reinterpret_cast<dir_entry*>(dbuf);
    dir_entry nde={};
    std::strncpy(nde.file_name, dname.c_str(), MAX_NAME_LEN);
    nde.file_name[MAX_NAME_LEN] = '\0';
    nde.type=TYPE_FILE; nde.first_blk=first; nde.size=src->size;
    nde.access_rights=src->access_rights;
    for(int i=0;i<slots;++i){
        if(!dents[i].file_name[0]){ dents[i]=nde; break; }
    }
    disk.write(ddir, dbuf);
    std::cout<<"File copied successfully\n";
    return 0;
}

// mv: rename or move into directory
int FS::mv(std::string sourcepath, std::string destpath) {
    // resolve src
    uint16_t sdir; std::string sname;
    if (resolve_path(sourcepath, sdir, sname)!=0 || sname.empty())
        return -1;
    uint8_t sb[BLOCK_SIZE]; disk.read(sdir,sb);
    auto *sents = reinterpret_cast<dir_entry*>(sb);
    int slots = BLOCK_SIZE/sizeof(dir_entry);
    int idx=-1;
    for(int i=0;i<slots;++i)
        if(sname==sents[i].file_name){ idx=i; break; }
    if(idx<0) return -1;

    // resolve dest
    uint16_t ddir; std::string dname;
    bool into_dir=false;
    if(resolve_path(destpath,ddir,dname)==0 && !dname.empty()){
        uint8_t db[BLOCK_SIZE]; disk.read(ddir,db);
        auto *dents = reinterpret_cast<dir_entry*>(db);
        for(int i=0;i<slots;++i){
            if(dname==dents[i].file_name && dents[i].type==TYPE_DIR){
                into_dir=true;
                ddir = dents[i].first_blk;
                dname = sname;
                break;
            }
            if(dname==dents[i].file_name) return -1;
        }
    } else {
        ddir = sdir;
        dname = destpath;
    }
    
    if (dname.length() > MAX_NAME_LEN) {
    std::cout << "Error: New name too long (max " << MAX_NAME_LEN << " characters)\n";
    return -1;
    }


    if(into_dir) {
        // remove from sdir, insert into ddir
        dir_entry temp = sents[idx];
        std::memset(&sents[idx],0,sizeof(dir_entry));
        disk.write(sdir,sb);

        uint8_t db[BLOCK_SIZE]; disk.read(ddir,db);
        auto *dents = reinterpret_cast<dir_entry*>(db);
        for(int i=0;i<slots;++i){
            if(!dents[i].file_name[0]){ dents[i]=temp; break; }
        }
        disk.write(ddir,db);
    } else {
        // rename in place
        std::strncpy(sents[idx].file_name, dname.c_str(), MAX_NAME_LEN);
        sents[idx].file_name[MAX_NAME_LEN] = '\0';

        disk.write(sdir,sb);
    }
    std::cout<<"File renamed successfully\n";
    return 0;
}

// rm: delete file or empty directory
int FS::rm(std::string filepath) {
    uint16_t dirblk; std::string name;
    if(resolve_path(filepath,dirblk,name)!=0 || name.empty()) return -1;
    uint8_t dbuf[BLOCK_SIZE]; disk.read(dirblk,dbuf);
    auto *ents = reinterpret_cast<dir_entry*>(dbuf);
    int slots= BLOCK_SIZE/sizeof(dir_entry);
    for(int i=0;i<slots;++i){
        if(name==ents[i].file_name){
            if(ents[i].type==TYPE_DIR){
                // check empty
                uint8_t b2[BLOCK_SIZE]; disk.read(ents[i].first_blk,b2);
                auto *sub = reinterpret_cast<dir_entry*>(b2);
                for(int j=2;j<slots;++j) if(sub[j].file_name[0]) return -1;
            }
            // free FAT chain
            int16_t blk=ents[i].first_blk;
            while(blk!=FAT_EOF){
                int16_t next=fat[blk];
                fat[blk]=FAT_FREE;
                blk=next;
            }
            std::memset(&ents[i],0,sizeof(dir_entry));
            // write FAT
            uint8_t fbuf[BLOCK_SIZE]={0};
            std::memcpy(fbuf, fat, sizeof(fat));
            disk.write(FAT_BLOCK, fbuf);
            // write dir
            disk.write(dirblk, dbuf);
            return 0;
        }
    }
    return -1;
}

// append: append file1 to file2
int FS::append(std::string f1, std::string f2) {
    // resolve both files
    uint16_t d1, d2; std::string n1, n2;
    if (resolve_path(f1, d1, n1) != 0 || n1.empty()) {
        std::cout << "Error: File not found: " << f1 << std::endl;
        return -1;
    }
    if (resolve_path(f2, d2, n2) != 0 || n2.empty()) {
        std::cout << "Error: File not found: " << f2 << std::endl;
        return -1;
    }

    uint8_t b1[BLOCK_SIZE], b2[BLOCK_SIZE];
    disk.read(d1, b1);
    disk.read(d2, b2);
    auto *e1 = reinterpret_cast<dir_entry*>(b1);
    auto *e2 = reinterpret_cast<dir_entry*>(b2);
    int slots = BLOCK_SIZE / sizeof(dir_entry);

    dir_entry *ent1 = nullptr, *ent2 = nullptr;
    for (int i = 0; i < slots; ++i) {
        if (n1 == e1[i].file_name) ent1 = &e1[i];
        if (n2 == e2[i].file_name) ent2 = &e2[i];
    }
    if (!ent1) {
        std::cout << "Error: File not found: " << f1 << std::endl;
        return -1;
    }
    if (!ent2) {
        std::cout << "Error: File not found: " << f2 << std::endl;
        return -1;
    }
    if (!(ent1->access_rights & READ)) {
        std::cout << "Error: Permission denied (no read access) on " << f1 << std::endl;
        return -1;
    }
    if (!(ent2->access_rights & WRITE)) {
        std::cout << "Error: Permission denied (no write access) on " << f2 << std::endl;
        return -1;
    }

    // Read entire content of f1
    std::vector<uint8_t> data;
    uint16_t blk = ent1->first_blk;
    int remaining = ent1->size;
    while (blk != FAT_EOF && remaining > 0) {
        uint8_t temp[BLOCK_SIZE];
        disk.read(blk, temp);
        int to_copy = std::min(remaining, BLOCK_SIZE);
        data.insert(data.end(), temp, temp + to_copy);
        remaining -= to_copy;
        blk = fat[blk];
    }

    // Now write this data to the end of f2
    uint16_t last_blk = ent2->first_blk;
    if (last_blk == FAT_EOF) {
        // Empty file, allocate first block
        last_blk = find_free_block();
        if (last_blk == -1) return -1;
        ent2->first_blk = last_blk;
        fat[last_blk] = FAT_EOF;
    } else {
        // Traverse to last block
        while (fat[last_blk] != FAT_EOF) {
            last_blk = fat[last_blk];
        }
    }

    // Determine where to start writing
    int f2_offset = ent2->size % BLOCK_SIZE;
    int f2_written = 0;
    blk = last_blk;
    uint8_t temp[BLOCK_SIZE];
    if (f2_offset != 0) {
        disk.read(blk, temp);
    } else {
        std::fill(temp, temp + BLOCK_SIZE, 0);
    }

    for (size_t i = 0; i < data.size(); ++i) {
        temp[f2_offset++] = data[i];
        ent2->size++;
        f2_written++;

        if (f2_offset == BLOCK_SIZE) {
            disk.write(blk, temp);
            // Allocate next block
            uint16_t new_blk = find_free_block();
            if (new_blk == -1) return -1;
            fat[blk] = new_blk;
            fat[new_blk] = FAT_EOF;
            blk = new_blk;
            f2_offset = 0;
            std::fill(temp, temp + BLOCK_SIZE, 0);
        }
    }

    // Write last block
    disk.write(blk, temp);

    // Update directory entry and write it back
    disk.write(d2, b2);

    return 0;
}

// mkdir: make single directory
int FS::mkdir(std::string dirpath) {
    uint16_t parent; std::string name;
    if(resolve_path(dirpath,parent,name)!=0||name.empty()) return -1;
    
    if (name.length() > MAX_NAME_LEN) {
    std::cout << "Error: Directory name too long (max " << MAX_NAME_LEN << " characters)\n";
    return -1;
    }


    uint8_t buf[BLOCK_SIZE]; disk.read(parent,buf);
    auto *ents = reinterpret_cast<dir_entry*>(buf);
    int slots=BLOCK_SIZE/sizeof(dir_entry);
    for(int i=0;i<slots;++i) if(name==ents[i].file_name) return -1;

    // allocate block
    int16_t nb=-1;
    for(int i=2;i<BLOCK_SIZE/2;++i) if(fat[i]==FAT_FREE){ nb=i; fat[i]=FAT_EOF; break; }
    if(nb<0) return -1;

    // init new dir
    dir_entry dot={}, dotdot={};
    std::strcpy(dot.file_name,"."); dot.first_blk=nb; dot.type=TYPE_DIR; dot.size=0; dot.access_rights=READ|WRITE|EXECUTE;
    std::strcpy(dotdot.file_name,".."); dotdot.first_blk=parent; dotdot.type=TYPE_DIR; dotdot.size=0; dotdot.access_rights=READ|WRITE|EXECUTE;
    {
        uint8_t b2[BLOCK_SIZE]={0};
        auto *dents = reinterpret_cast<dir_entry*>(b2);
        dents[0]=dot; dents[1]=dotdot;
        disk.write(nb,b2);
    }
    // add to parent
    for(int i=0;i<slots;++i){
        if(!ents[i].file_name[0]){
            std::strncpy(ents[i].file_name, name.c_str(), MAX_NAME_LEN);
            ents[i].file_name[MAX_NAME_LEN] = '\0';
            ents[i].first_blk=nb; ents[i].type=TYPE_DIR; ents[i].size=0; ents[i].access_rights=READ|WRITE|EXECUTE;
            break;
        }
    }
    disk.write(parent,buf);
    return 0;
}

// cd: change directory
int FS::cd(std::string dirpath) {
    if(dirpath==".") return 0;
    if(dirpath==".."){
        current_dir = get_parent_directory(current_dir);
        return 0;
    }
    uint16_t parent; std::string name;
    // trick: append "/." so last resolves as dir
    std::string p = (dirpath.back()=='/'?dirpath:dirpath+"/.") ;
    if(resolve_path(p,parent,name)!=0) return -1;
    current_dir = parent;
    return 0;
}

// pwd: print path
int FS::pwd() {
    std::vector<std::string> parts;
    uint16_t dir = current_dir;
    while(dir != ROOT_BLOCK) {
        uint16_t par = get_parent_directory(dir);
        uint8_t buf[BLOCK_SIZE]; disk.read(par,buf);
        auto *ents = reinterpret_cast<dir_entry*>(buf);
        int slots=BLOCK_SIZE/sizeof(dir_entry);
        for(int i=0;i<slots;++i){
            if(ents[i].first_blk==dir && std::string(ents[i].file_name)!="." && std::string(ents[i].file_name)!=".."){
                parts.push_back(ents[i].file_name);
                break;
            }
        }
        dir=par;
    }
    std::cout<<"/";
    for(auto it=parts.rbegin(); it!=parts.rend(); ++it)
        std::cout<<*it<<"/";
    std::cout<<"\n";
    return 0;
}

// chmod: change access bits
int FS::chmod(std::string accessrights, std::string filepath) {
    uint16_t dirblk; std::string name;
    if(resolve_path(filepath,dirblk,name)!=0||name.empty()) return -1;
    uint8_t buf[BLOCK_SIZE]; disk.read(dirblk,buf);
    auto *ents = reinterpret_cast<dir_entry*>(buf);
    int slots=BLOCK_SIZE/sizeof(dir_entry);
    int val = std::stoi(accessrights, nullptr, 8 /*octal*/);
    for(int i=0;i<slots;++i){
        if(name==ents[i].file_name){
            ents[i].access_rights = val;
            disk.write(dirblk,buf);
            return 0;
        }
    }
    return -1;
}

// helpers:
bool FS::is_directory(uint16_t dir_block) {
    uint8_t buf[BLOCK_SIZE]; if(disk.read(dir_block,buf)!=0) return false;
    auto *ents = reinterpret_cast<dir_entry*>(buf);
    return ents[0].type == TYPE_DIR;
}
uint16_t FS::get_parent_directory(uint16_t dir_block) {
    uint8_t buf[BLOCK_SIZE]; if(disk.read(dir_block,buf)!=0) return ROOT_BLOCK;
    auto *ents = reinterpret_cast<dir_entry*>(buf);
    int slots=BLOCK_SIZE/sizeof(dir_entry);
    for(int i=0;i<slots;++i){
        if(std::string(ents[i].file_name) == "..")
            return ents[i].first_blk;
    }
    return ROOT_BLOCK;
}

int FS::find_free_block() {
    for (int i = 2; i < BLOCK_SIZE / 2; ++i) { // Skip ROOT and FAT
        if (fat[i] == FAT_FREE)
            return i;
    }
    return -1; // Disk full
}
