/*

Copyright (c) 2003-2016, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/aux_/storage_utils.hpp"
#include "libtorrent/file_storage.hpp"
#include "libtorrent/alloca.hpp"
#include "libtorrent/file.hpp" // for count_bufs
#include "libtorrent/part_file.hpp"

#include <set>
#include <string>

namespace libtorrent
{
	int copy_bufs(span<iovec_t const> bufs, int bytes, span<iovec_t> target)
	{
		int size = 0;
		for (int i = 0;; i++)
		{
			target[i] = bufs[i];
			size += int(bufs[i].iov_len);
			if (size >= bytes)
			{
				target[i].iov_len -= size - bytes;
				return i + 1;
			}
		}
	}

	span<iovec_t> advance_bufs(span<iovec_t> bufs, int bytes)
	{
		int size = 0;
		for (;;)
		{
			size += int(bufs.front().iov_len);
			if (size >= bytes)
			{
				bufs.front().iov_base = reinterpret_cast<char*>(bufs.front().iov_base)
					+ bufs.front().iov_len - (size - bytes);
				bufs.front().iov_len = size - bytes;
				return bufs;
			}
			bufs = bufs.subspan(1);
		}
	}

#if TORRENT_USE_ASSERTS
	namespace {

	int count_bufs(span<iovec_t const> bufs, int bytes)
	{
		int size = 0;
		int count = 1;
		if (bytes == 0) return 0;
		for (auto i = bufs.begin();; ++i, ++count)
		{
			size += int(i->iov_len);
			if (size >= bytes) return count;
		}
	}

	}
#endif

	// much of what needs to be done when reading and writing is buffer
	// management and piece to file mapping. Most of that is the same for reading
	// and writing. This function is a template, and the fileop decides what to
	// do with the file and the buffers.
	int readwritev(file_storage const& files, span<iovec_t const> const bufs
		, piece_index_t const piece, const int offset, fileop& op
		, storage_error& ec)
	{
		TORRENT_ASSERT(piece >= piece_index_t(0));
		TORRENT_ASSERT(piece < files.end_piece());
		TORRENT_ASSERT(offset >= 0);
		TORRENT_ASSERT(bufs.size() > 0);

		const int size = bufs_size(bufs);
		TORRENT_ASSERT(size > 0);

		// find the file iterator and file offset
		std::int64_t const torrent_offset = static_cast<int>(piece) * std::int64_t(files.piece_length()) + offset;
		file_index_t file_index = files.file_index_at_offset(torrent_offset);
		TORRENT_ASSERT(torrent_offset >= files.file_offset(file_index));
		TORRENT_ASSERT(torrent_offset < files.file_offset(file_index) + files.file_size(file_index));
		std::int64_t file_offset = torrent_offset - files.file_offset(file_index);

		// the number of bytes left before this read or write operation is
		// completely satisfied.
		int bytes_left = size;

		TORRENT_ASSERT(bytes_left >= 0);

		// copy the iovec array so we can use it to keep track of our current
		// location by updating the head base pointer and size. (see
		// advance_bufs())
		TORRENT_ALLOCA(current_buf, iovec_t, bufs.size());
		copy_bufs(bufs, size, current_buf);
		TORRENT_ASSERT(count_bufs(current_buf, size) == int(bufs.size()));

		TORRENT_ALLOCA(tmp_buf, iovec_t, bufs.size());

		// the number of bytes left to read in the current file (specified by
		// file_index). This is the minimum of (file_size - file_offset) and
		// bytes_left.
		int file_bytes_left;

		while (bytes_left > 0)
		{
			file_bytes_left = bytes_left;
			if (file_offset + file_bytes_left > files.file_size(file_index))
				file_bytes_left = (std::max)(static_cast<int>(files.file_size(file_index) - file_offset), 0);

			// there are no bytes left in this file, move to the next one
			// this loop skips over empty files
			while (file_bytes_left == 0)
			{
				++file_index;
				file_offset = 0;
				TORRENT_ASSERT(file_index < files.end_file());

				// this should not happen. bytes_left should be clamped by the total
				// size of the torrent, so we should never run off the end of it
				if (file_index >= files.end_file()) return size;

				file_bytes_left = bytes_left;
				if (file_offset + file_bytes_left > files.file_size(file_index))
					file_bytes_left = (std::max)(static_cast<int>(files.file_size(file_index) - file_offset), 0);
			}

			// make a copy of the iovec array that _just_ covers the next
			// file_bytes_left bytes, i.e. just this one operation
			int tmp_bufs_used = copy_bufs(current_buf, file_bytes_left, tmp_buf);

			int bytes_transferred = op.file_op(file_index, file_offset
				, tmp_buf.first(tmp_bufs_used), ec);
			if (ec) return -1;

			// advance our position in the iovec array and the file offset.
			current_buf = advance_bufs(current_buf, bytes_transferred);
			bytes_left -= bytes_transferred;
			file_offset += bytes_transferred;

			TORRENT_ASSERT(count_bufs(current_buf, bytes_left) <= int(bufs.size()));

			// if the file operation returned 0, we've hit end-of-file. We're done
			if (bytes_transferred == 0)
			{
				if (file_bytes_left > 0 )
				{
					// fill in this information in case the caller wants to treat
					// a short-read as an error
					ec.file(file_index);
				}
				return size - bytes_left;
			}
		}
		return size;
	}

	std::pair<status_t, std::string> move_storage(file_storage const& f
		, std::string const& save_path
		, std::string const& destination_save_path
		, part_file* pf
		, int const flags, storage_error& ec)
	{
		status_t ret = status_t::no_error;
		std::string const new_save_path = complete(destination_save_path);

		// check to see if any of the files exist
		if (flags == fail_if_exist)
		{
			file_status s;
			error_code err;
			stat_file(new_save_path, &s, err);
			if (err != boost::system::errc::no_such_file_or_directory)
			{
				// the directory exists, check all the files
				for (file_index_t i(0); i < f.end_file(); ++i)
				{
					// files moved out to absolute paths are ignored
					if (f.file_absolute_path(i)) continue;

					stat_file(f.file_path(i, new_save_path), &s, err);
					if (err != boost::system::errc::no_such_file_or_directory)
					{
						ec.ec = err;
						ec.file(i);
						ec.operation = storage_error::stat;
						return { status_t::file_exist, save_path };
					}
				}
			}
		}

		{
			file_status s;
			error_code err;
			stat_file(new_save_path, &s, err);
			if (err == boost::system::errc::no_such_file_or_directory)
			{
				err.clear();
				create_directories(new_save_path, err);
				if (err)
				{
					ec.ec = err;
					ec.file(file_index_t(-1));
					ec.operation = storage_error::mkdir;
					return { status_t::fatal_disk_error, save_path };
				}
			}
			else if (err)
			{
				ec.ec = err;
				ec.file(file_index_t(-1));
				ec.operation = storage_error::stat;
				return { status_t::fatal_disk_error, save_path };
			}
		}

		// indices of all files we ended up copying. These need to be deleted
		// later
		aux::vector<bool, file_index_t> copied_files(std::size_t(f.num_files()), false);

		file_index_t i;
		error_code e;
		for (i = file_index_t(0); i < f.end_file(); ++i)
		{
			// files moved out to absolute paths are not moved
			if (f.file_absolute_path(i)) continue;

			std::string const old_path = combine_path(save_path, f.file_path(i));
			std::string const new_path = combine_path(new_save_path, f.file_path(i));

			if (flags == dont_replace && exists(new_path))
			{
				if (ret == status_t::no_error) ret = status_t::need_full_check;
				continue;
			}

			// TODO: ideally, if we end up copying files because of a move across
			// volumes, the source should not be deleted until they've all been
			// copied. That would let us rollback with higher confidence.
			move_file(old_path, new_path, e);

			// if the source file doesn't exist. That's not a problem
			// we just ignore that file
			if (e == boost::system::errc::no_such_file_or_directory)
				e.clear();
			else if (e
				&& e != boost::system::errc::invalid_argument
				&& e != boost::system::errc::permission_denied)
			{
				// moving the file failed
				// on OSX, the error when trying to rename a file across different
				// volumes is EXDEV, which will make it fall back to copying.
				e.clear();
				copy_file(old_path, new_path, e);
				if (!e) copied_files[i] = true;
			}

			if (e)
			{
				ec.ec = e;
				ec.file(i);
				ec.operation = storage_error::rename;
				break;
			}
		}

		if (!e && pf)
		{
			pf->move_partfile(new_save_path, e);
			if (e)
			{
				ec.ec = e;
				ec.file(file_index_t(-1));
				ec.operation = storage_error::partfile_move;
			}
		}

		if (e)
		{
			// rollback
			while (--i >= file_index_t(0))
			{
				// files moved out to absolute paths are not moved
				if (f.file_absolute_path(i)) continue;

				// if we ended up copying the file, don't do anything during
				// roll-back
				if (copied_files[i]) continue;

				std::string const old_path = combine_path(save_path, f.file_path(i));
				std::string const new_path = combine_path(new_save_path, f.file_path(i));

				// ignore errors when rolling back
				error_code ignore;
				move_file(new_path, old_path, ignore);
			}

			return { status_t::fatal_disk_error, save_path };
		}

		// TODO: 2 technically, this is where the transaction of moving the files
		// is completed. This is where the new save_path should be committed. If
		// there is an error in the code below, that should not prevent the new
		// save path to be set. Maybe it would make sense to make the save_path
		// an in-out parameter

		std::set<std::string> subdirs;
		for (i = file_index_t(0); i < f.end_file(); ++i)
		{
			// files moved out to absolute paths are not moved
			if (f.file_absolute_path(i)) continue;

			if (has_parent_path(f.file_path(i)))
				subdirs.insert(parent_path(f.file_path(i)));

			// if we ended up renaming the file instead of moving it, there's no
			// need to delete the source.
			if (copied_files[i] == false) continue;

			std::string const old_path = combine_path(save_path, f.file_path(i));

			// we may still have some files in old save_path
			// eg. if (flags == dont_replace && exists(new_path))
			// ignore errors when removing
			error_code ignore;
			remove(old_path, ignore);
		}

		for (std::string const& s : subdirs)
		{
			error_code err;
			std::string subdir = combine_path(save_path, s);

			while (subdir != save_path && !err)
			{
				remove(subdir, err);
				subdir = parent_path(subdir);
			}
		}

		return { ret, new_save_path };
	}

}

