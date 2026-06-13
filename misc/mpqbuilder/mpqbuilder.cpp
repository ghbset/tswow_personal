/*
 * This file is part of tswow (https://github.com/tswow)
 *
 * Copyright (C) 2020 tswow <https://github.com/tswow/>
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include <StormLib.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <thread>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

inline bool exists(std::string const& name) {
    std::ifstream f(name.c_str());
    return f.good();
}

bool clearFile(std::string const& file, const char* errorMsg)
{
    if (exists(file))
    {
        remove(file.c_str());
        if (exists(file))
        {
            std::cout << errorMsg << file << "\n";
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cout << "Usage: mpqbuilder filelist outputfile";
        return -1;
    }

    // 1. Read input file
    std::string fileList = argv[1];
    if (!exists(fileList))
    {
       std::cerr << "File list " << fileList << " does not exist\n";
    }
    std::ifstream is(fileList);
    std::string line;
    std::vector<std::pair<std::string,std::string>> files;
    std::vector<std::string> errors;
    while (std::getline(is, line))
    {
        size_t fst = line.find_first_of('\t');
        if (fst == std::string::npos)
        {
            errors.push_back("Malformed line:" + line);
            continue;
        }

        std::string src = line.substr(0, fst);
        std::string dst = line.substr(fst + 1);

        if (!exists(src))
        {
            errors.push_back("Missing file:" + src);
            continue;
        }
        files.push_back(std::make_pair(src,dst));
    }

    if (errors.size() > 0)
    {
        std::cerr << "Errors encountered when reading file list:\n";
        for (std::string const& error : errors)
        {
           std::cerr << error << "\n";
        }
        return -1;
    }

    if(files.size() == 0)
    {
        std::cerr << "Tried creating an MPQ with no files\n";
        return -1;
    }

    if (files.size() > 0x80000)
    {
        std::cerr << "Tried to create an MPQ with more than the maximum supported " << 0x80000 << " files\n";
        return -1;
    }

    // 2. Write MPQ
    struct TempFile {
       std::string m_file;
       TempFile(std::string const& file): m_file(file){}
       ~TempFile() {
           clearFile(m_file, "Failed to remove temp file ");
       }
    } temp(std::string(argv[2])+".temp");

    if (!clearFile(temp.m_file, "Failed to remove old temp file ")) return -1;

    HANDLE handle = NULL;
    size_t power = 4;
    while (power < files.size()) power <<= 1;
    // MPQ_CREATE_ARCHIVE_V2 (0x01000000) selects the 64-bit-offset format
    // that supports archives larger than 4 GB. The default flag (V1) caps
    // the archive at 4 GB and segfaults at the boundary on large datasets
    // like openazeroth's WORLD/ tree. WoW 3.3.5a clients support v2.
    if (!SFileCreateArchive(temp.m_file.c_str(), MPQ_CREATE_ARCHIVE_V2, power, &handle))
    {
        std::cerr << "Failed to create output mpq file " << temp.m_file << "\n";
        return -1;
    }

    // --- Parallel page-cache pre-warm ----------------------------------
    // SFileAddFile reads each source file synchronously. For ~1.4M files
    // (the package-client workload), the per-file open/read disk-seek
    // overhead dominates wall time even though StormLib's compression is
    // fast. Fan out N worker threads to open + drain each source file's
    // bytes into the kernel page cache before the sequential MPQ-write
    // loop runs; the sequential reads then hit cache.
    //
    // No threading inside StormLib: SFileAddFile is still called
    // sequentially below. This is a pure I/O-prefetch optimisation.
    {
        size_t worker_count = std::thread::hardware_concurrency();
        if (worker_count < 2) worker_count = 2;
        if (worker_count > 16) worker_count = 16;   // diminishing returns past this
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        std::atomic<size_t> next{0};
        for (size_t w = 0; w < worker_count; w++) {
            workers.emplace_back([&]() {
                char buf[64 * 1024];
                while (true) {
                    size_t i = next.fetch_add(1);
                    if (i >= files.size()) break;
                    int fd = ::open(files[i].first.c_str(), O_RDONLY);
                    if (fd < 0) continue;   // skipped at AddFile time too
                    while (::read(fd, buf, sizeof(buf)) > 0) {}
                    ::close(fd);
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    for (auto const& pair : files)
    {
        SFileAddFile(handle, pair.first.c_str(), pair.second.c_str(), 0);
    }
    SFileFlushArchive(handle);
    SFileCloseArchive(handle);

    // 3. Save to real output file
    std::string outputFile = argv[2];
    if (!clearFile(outputFile, "Failed to remove old mpq file "));

    std::ifstream  src(temp.m_file, std::ios::binary);
    std::ofstream  dst(outputFile, std::ios::binary);
    dst << src.rdbuf();
    return 0;
}