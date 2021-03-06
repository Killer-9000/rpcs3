#include "stdafx.h"

#include "TAR.h"

#include "util/asm.hpp"

#include <cmath>
#include <cstdlib>
#include <charconv>

LOG_CHANNEL(tar_log, "TAR");

tar_object::tar_object(const fs::file& file)
	: m_file(file)
{
}

TARHeader tar_object::read_header(u64 offset) const
{
	TARHeader header{};

	if (m_file.seek(offset) != offset)
	{
		return header;
	}

	if (!m_file.read(header))
	{
		std::memset(&header, 0, sizeof(header));
	}

	return header;
}

u64 octal_text_to_u64(std::string_view sv)
{
	u64 i = -1;
	const auto ptr = std::from_chars(sv.data(), sv.data() + sv.size(), i, 8).ptr;

	// Range must be terminated with either NUL or space
	if (ptr == sv.data() + sv.size() || (*ptr && *ptr != ' '))
	{
		i = -1;
	}

	return i;
}

std::vector<std::string> tar_object::get_filenames()
{
	std::vector<std::string> vec;
	get_file("");
	for (auto it = m_map.cbegin(); it != m_map.cend(); ++it)
	{
		vec.push_back(it->first);
	}
	return vec;
}

fs::file tar_object::get_file(const std::string& path)
{
	if (!m_file) return fs::file();

	if (auto it = m_map.find(path); it != m_map.end())
	{
		u64 size = 0;
		std::memcpy(&size, it->second.second.size, sizeof(size));
		std::vector<u8> buf(size);
		m_file.seek(it->second.first);
		m_file.read(buf, size);
		return fs::make_stream(std::move(buf));
	}
	else //continue scanning from last file entered
	{
		const u64 max_size = m_file.size();

		while (largest_offset < max_size)
		{
			TARHeader header = read_header(largest_offset);

			u64 size = -1;

			if (header.name[0] && std::memcmp(header.magic, "ustar", 5) == 0)
			{
				const std::string_view size_sv{header.size, std::size(header.size)};

				size = octal_text_to_u64(size_sv);

				// Check for overflows and if surpasses file size
				if (size + 512 > size && max_size >= size + 512 && max_size - size - 512 >= largest_offset)
				{
					// Cache size in native u64 format
					static_assert(sizeof(size) < sizeof(header.size));
					std::memcpy(header.size, &size, 8);

					// Save header andd offset
					m_map.insert_or_assign(header.name, std::make_pair(largest_offset + 512, header));
				}
				else
				{
					// Invalid
					size = -1;
					tar_log.error("tar_object::get_file() failed to convert header.size=%s, filesize=0x%x", size_sv, max_size);
				}
			}
			else
			{
				tar_log.trace("tar_object::get_file() failed to parse header: offset=0x%x, filesize=0x%x", largest_offset, max_size);
			}

			if (size == umax)
			{
				size = 0;
				header.name[0] = '\0'; // Ensure path will not be equal
			}

			if (!path.empty() && path == header.name)
			{
				// Path is equal, read file and advance offset to start of next block
				std::vector<u8> buf(size);

				if (m_file.read(buf, size))
				{
					largest_offset += utils::align(size, 512) + 512;
					return fs::make_stream(std::move(buf));
				}

				tar_log.error("tar_object::get_file() failed to read file entry %s (size=0x%x)", header.name, size);
				size = 0;
			}

			// Advance offset to next block
			largest_offset += utils::align(size, 512) + 512;
		}

		return fs::file();
	}
}

bool tar_object::extract(std::string path, std::string ignore)
{
	if (!m_file) return false;

	get_file(""); // Make sure we have scanned all files

	for (auto& iter : m_map)
	{
		const TARHeader& header = iter.second.second;
		const std::string& name = iter.first;

		std::string result = path + name;

		if (result.compare(path.size(), ignore.size(), ignore) == 0)
		{
			result.erase(path.size(), ignore.size());
		}

		switch (header.filetype)
		{
		case '\0':
		case '0':
		{
			auto data = get_file(name).release();

			fs::file file(result, fs::rewrite);

			if (file)
			{
				file.write(static_cast<fs::container_stream<std::vector<u8>>*>(data.get())->obj);
				tar_log.notice("TAR Loader: written file %s", name);
				break;
			}

			const auto old_error = fs::g_tls_error;
			tar_log.error("TAR Loader: failed to write file %s (%s) (fs::exists=%s)", name, old_error, fs::exists(result));
			return false;
		}

		case '5':
		{
			if (!fs::create_path(result))
			{
				tar_log.error("TAR Loader: failed to create directory %s (%s)", name, fs::g_tls_error);
				return false;
			}

			break;
		}

		default:
			tar_log.error("TAR Loader: unknown file type: 0x%x", header.filetype);
			return false;
		}
	}
	return true;
}
