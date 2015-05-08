/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "path.h"
#include "path_w32.h"
#include "utf-conv.h"
#include "posix.h"
#include "reparse.h"
#include "dir.h"

#define PATH__NT_NAMESPACE     L"\\\\?\\"
#define PATH__NT_NAMESPACE_LEN 4

#define PATH__ABSOLUTE_LEN     3

#define path__is_dirsep(p) ((p) == '/' || (p) == '\\')

#define path__is_absolute(p) \
	(git__isalpha((p)[0]) && (p)[1] == ':' && ((p)[2] == '\\' || (p)[2] == '/'))

#define path__is_nt_namespace(p) \
	(((p)[0] == '\\' && (p)[1] == '\\' && (p)[2] == '?' && (p)[3] == '\\') || \
	 ((p)[0] == '/' && (p)[1] == '/' && (p)[2] == '?' && (p)[3] == '/'))

#define path__is_unc(p) \
	(((p)[0] == '\\' && (p)[1] == '\\') || ((p)[0] == '/' && (p)[1] == '/'))

#define PATH__MAX_UNC_LEN (32767)

GIT_INLINE(int) path__cwd(wchar_t *path, int size)
{
	int len;

	if ((len = GetCurrentDirectoryW(size, path)) == 0) {
		errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : ENOENT;
		return -1;
	} else if (len > size) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* The Win32 APIs may return "\\?\" once you've used it first.
	 * But it may not.  What a gloriously predictible API!
	 */
	if (wcsncmp(path, PATH__NT_NAMESPACE, PATH__NT_NAMESPACE_LEN))
		return len;

	len -= PATH__NT_NAMESPACE_LEN;

	memmove(path, path + PATH__NT_NAMESPACE_LEN, sizeof(wchar_t) * len);
	return len;
}

static wchar_t *path__skip_server(wchar_t *path)
{
	wchar_t *c;

	for (c = path; *c; c++) {
		if (path__is_dirsep(*c))
			return c + 1;
	}

	return c;
}

static wchar_t *path__skip_prefix(wchar_t *path)
{
	if (path__is_nt_namespace(path)) {
		path += PATH__NT_NAMESPACE_LEN;

		if (wcsncmp(path, L"UNC\\", 4) == 0)
			path = path__skip_server(path + 4);
		else if (path__is_absolute(path))
			path += PATH__ABSOLUTE_LEN;
	} else if (path__is_absolute(path)) {
		path += PATH__ABSOLUTE_LEN;
	} else if (path__is_unc(path)) {
		path = path__skip_server(path + 2);
	}

	return path;
}

int git_win32_path_canonicalize(git_win32_path path)
{
	wchar_t *base, *from, *to, *next;
	size_t len;

	base = to = path__skip_prefix(path);

	/* Unposixify if the prefix */
	for (from = path; from < to; from++) {
		if (*from == L'/')
			*from = L'\\';
	}

	while (*from) {
		for (next = from; *next; ++next) {
			if (*next == L'/') {
				*next = L'\\';
				break;
			}

			if (*next == L'\\')
				break;
		}

		len = next - from;

		if (len == 1 && from[0] == L'.')
			/* do nothing with singleton dot */;

		else if (len == 2 && from[0] == L'.' && from[1] == L'.') {
			if (to == base) {
				/* no more path segments to strip, eat the "../" */
				if (*next == L'\\')
					len++;

				base = to;
			} else {
				/* back up a path segment */
				while (to > base && to[-1] == L'\\') to--;
				while (to > base && to[-1] != L'\\') to--;
			}
		} else {
			if (*next == L'\\' && *from != L'\\')
				len++;

			if (to != from)
				memmove(to, from, sizeof(wchar_t) * len);

			to += len;
		}

		from += len;

		while (*from == L'\\') from++;
	}

	/* Strip trailing backslashes */
	while (to > base && to[-1] == L'\\') to--;

	*to = L'\0';

	return (to - path);
}

int git_win32_path__cwd(wchar_t *out, size_t len)
{
	int cwd_len;

	if ((cwd_len = path__cwd(out, len)) < 0)
		return -1;

	/* UNC paths */
	if (wcsncmp(L"\\\\", out, 2) == 0) {
		/* Our buffer must be at least 5 characters larger than the
		 * current working directory:  we swallow one of the leading
		 * '\'s, but we we add a 'UNC' specifier to the path, plus
		 * a trailing directory separator, plus a NUL.
		 */
		if (cwd_len > MAX_PATH - 4) {
			errno = ENAMETOOLONG;
			return -1;
		}

		memmove(out+2, out, sizeof(wchar_t) * cwd_len);
		out[0] = L'U';
		out[1] = L'N';
		out[2] = L'C';

		cwd_len += 2;
	}

	/* Our buffer must be at least 2 characters larger than the current
	 * working directory.  (One character for the directory separator,
	 * one for the null.
	 */
	else if (cwd_len > MAX_PATH - 2) {
		errno = ENAMETOOLONG;
		return -1;
	}

	return cwd_len;
}

int git_win32_path_from_utf8(git_win32_path out, const char *src)
{
	wchar_t *dest = out;

	/* All win32 paths are in NT-prefixed format, beginning with "\\?\". */
	memcpy(dest, PATH__NT_NAMESPACE, sizeof(wchar_t) * PATH__NT_NAMESPACE_LEN);
	dest += PATH__NT_NAMESPACE_LEN;

	/* See if this is an absolute path (beginning with a drive letter) */
	if (path__is_absolute(src)) {
		if (git__utf8_to_16(dest, MAX_PATH, src) < 0)
			return -1;
	}
	/* File-prefixed NT-style paths beginning with \\?\ */
	else if (path__is_nt_namespace(src)) {
		/* Skip the NT prefix, the destination already contains it */
		if (git__utf8_to_16(dest, MAX_PATH, src + PATH__NT_NAMESPACE_LEN) < 0)
			return -1;
	}
	/* UNC paths */
	else if (path__is_unc(src)) {
		memcpy(dest, L"UNC\\", sizeof(wchar_t) * 4);
		dest += 4;

		/* Skip the leading "\\" */
		if (git__utf8_to_16(dest, MAX_PATH - 2, src + 2) < 0)
			return -1;
	}
	/* Absolute paths omitting the drive letter */
	else if (src[0] == '\\' || src[0] == '/') {
		if (path__cwd(dest, MAX_PATH) < 0)
			return -1;

		if (!path__is_absolute(dest)) {
			errno = ENOENT;
			return -1;
		}

		/* Skip the drive letter specification ("C:") */	
		if (git__utf8_to_16(dest + 2, MAX_PATH - 2, src) < 0)
			return -1;
	}
	/* Relative paths */
	else {
		int cwd_len;

		if ((cwd_len = git_win32_path__cwd(dest, MAX_PATH)) < 0)
			return -1;

		dest[cwd_len++] = L'\\';

		if (git__utf8_to_16(dest + cwd_len, MAX_PATH - cwd_len, src) < 0)
			return -1;
	}

	return git_win32_path_canonicalize(out);
}

int git_win32_path_to_utf8(git_win32_utf8_path dest, const wchar_t *src)
{
	char *out = dest;
	int len;

	/* Strip NT namespacing "\\?\" */
	if (path__is_nt_namespace(src)) {
		src += 4;

		/* "\\?\UNC\server\share" -> "\\server\share" */
		if (wcsncmp(src, L"UNC\\", 4) == 0) {
			src += 4;

			memcpy(dest, "\\\\", 2);
			out = dest + 2;
		}
	}

	if ((len = git__utf16_to_8(out, GIT_WIN_PATH_UTF8, src)) < 0)
		return len;

	git_path_mkposix(dest);

	return len;
}

char *git_win32_path_8dot3_name(const char *path)
{
	git_win32_path longpath, shortpath;
	wchar_t *start;
	char *shortname;
	int len, namelen = 1;

	if (git_win32_path_from_utf8(longpath, path) < 0)
		return NULL;

	len = GetShortPathNameW(longpath, shortpath, GIT_WIN_PATH_UTF16);

	while (len && shortpath[len-1] == L'\\')
		shortpath[--len] = L'\0';

	if (len == 0 || len >= GIT_WIN_PATH_UTF16)
		return NULL;

	for (start = shortpath + (len - 1);
		start > shortpath && *(start-1) != '/' && *(start-1) != '\\';
		start--)
		namelen++;

	/* We may not have actually been given a short name.  But if we have,
	 * it will be in the ASCII byte range, so we don't need to worry about
	 * multi-byte sequences and can allocate naively.
	 */
	if (namelen > 12 || (shortname = git__malloc(namelen + 1)) == NULL)
		return NULL;

	if ((len = git__utf16_to_8(shortname, namelen + 1, start)) < 0)
		return NULL;

	return shortname;
}

#if !defined(__MINGW32__)
int git_win32_path_dirload_with_stat(
	const char *path,
	size_t prefix_len,
	unsigned int flags,
	const char *start_stat,
	const char *end_stat,
	git_vector *contents)
{
	int error = 0;
	git_path_with_stat *ps;
	git_win32_path pathw;
	DIR *dir;
	int(*strncomp)(const char *a, const char *b, size_t sz);
	size_t cmp_len;
	size_t start_len = start_stat ? strlen(start_stat) : 0;
	size_t end_len = end_stat ? strlen(end_stat) : 0;
	size_t path_size = strlen(path);
	const char *repo_path = path + prefix_len;
	size_t repo_path_len = strlen(repo_path);
	char work_path[PATH__MAX_UNC_LEN];
	git_win32_path target;
	size_t path_len;
	int fMode;

	if (!git_win32__findfirstfile_filter(pathw, path)) {
		error = -1;
		giterr_set(GITERR_OS, "Could not parse the path '%s'", path);
		goto clean_up_and_exit;
	}

	strncomp = (flags & GIT_PATH_DIR_IGNORE_CASE) != 0 
		       ? git__strncasecmp 
		       : git__strncmp;

	/* use of FIND_FIRST_EX_LARGE_FETCH flag in the FindFirstFileExW call could benefit perormance
	 * here when querying large repositories on Windows 7 (0x0600) or newer versions of Windows.
	 * doing so could introduce compatibility issues on older versions of Windows. */
	dir = git__calloc(1, sizeof(DIR));
	dir->h = FindFirstFileExW(pathw, FindExInfoBasic, &dir->f, FindExSearchNameMatch, NULL, 0);
	dir->first = 1;
	if (dir->h == INVALID_HANDLE_VALUE) {
		error = -1;
		giterr_set(GITERR_OS, "Could not open directory '%s'", path);
		goto clean_up_and_exit;
	}
	
	if (repo_path_len > PATH__MAX_UNC_LEN) {
		error = -1;
		giterr_set(GITERR_OS, "Could not open directory '%s'", path);
		goto clean_up_and_exit;
	}

	memcpy(work_path, repo_path, repo_path_len);

	while (dir) {
		if (!git_path_is_dot_or_dotdotW(dir->f.cFileName)) {
			path_len = git__utf16_to_8(work_path + repo_path_len, ARRAYSIZE(work_path) - repo_path_len, dir->f.cFileName);

			work_path[path_len + repo_path_len] = '\0';
			path_len = path_len + repo_path_len;

			cmp_len = min(start_len, path_len);
			if (!(cmp_len && strncomp(work_path, start_stat, cmp_len) < 0)) {
				cmp_len = min(end_len, path_len);
				if (!(cmp_len && strncomp(work_path, end_stat, cmp_len) > 0)) {
					fMode = S_IREAD;

					if (dir->f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
						fMode |= S_IFDIR;
					else
						fMode |= S_IFREG;

					if (!(dir->f.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
						fMode |= S_IWRITE;

					ps = git__calloc(1, sizeof(git_path_with_stat) + path_len + 2);
					memcpy(ps->path, work_path, path_len + 1);
					ps->path_len = path_len;
					ps->st.st_atime = filetime_to_time_t(&dir->f.ftLastAccessTime);
					ps->st.st_ctime = filetime_to_time_t(&dir->f.ftCreationTime);
					ps->st.st_mtime = filetime_to_time_t(&dir->f.ftLastWriteTime);
					ps->st.st_size = dir->f.nFileSizeHigh;
					ps->st.st_size <<= 32;
					ps->st.st_size |= dir->f.nFileSizeLow;
					ps->st.st_dev = ps->st.st_rdev = (_getdrive() - 1);
					ps->st.st_mode = (mode_t)fMode;
					ps->st.st_ino = 0;
					ps->st.st_gid = 0;
					ps->st.st_uid = 0;
					ps->st.st_nlink = 1;

					if (dir->f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
						if (git_win32_path_readlink_w(target, dir->f.cFileName) >= 0) {
							ps->st.st_mode = (ps->st.st_mode & ~S_IFMT) | S_IFLNK;

							/* st_size gets the UTF-8 length of the target name, in bytes,
							 * not counting the NULL terminator */
							if ((ps->st.st_size = git__utf16_to_8(NULL, 0, target)) < 0) {
								error = -1;
								giterr_set(GITERR_OS, "Could not manage reparse link '%s'", dir->f.cFileName);
								goto clean_up_and_exit;
							}
						}
					}

					if (S_ISDIR(ps->st.st_mode)) {
						ps->path[ps->path_len++] = '/';
						ps->path[ps->path_len] = '\0';
					} else if (!S_ISREG(ps->st.st_mode) && !S_ISLNK(ps->st.st_mode)) {
						git__free(ps);
						ps = NULL;
					}

					if (ps)
						git_vector_insert(contents, ps);
				}
			}
		}

		memset(&dir->f, 0, sizeof(git_path_with_stat));
		dir->first = 0;

		if (!FindNextFileW(dir->h, &dir->f)) {
			if (GetLastError() == ERROR_NO_MORE_FILES)
				break;
			else {
				error = -1;
				giterr_set(GITERR_OS, "Could not get attributes for file in '%s'", path);
				goto clean_up_and_exit;
			}
		}
	}

	/* sort now that directory suffix is added */
	git_vector_sort(contents);

clean_up_and_exit:

	if (dir) {
		FindClose(dir->h);
		free(dir);
	}

	return error;
}
#endif

static bool path_is_volume(wchar_t *target, size_t target_len)
{
	return (target_len && wcsncmp(target, L"\\??\\Volume{", 11) == 0);
}

/* On success, returns the length, in characters, of the path stored in dest.
* On failure, returns a negative value. */
int git_win32_path_readlink_w(git_win32_path dest, const git_win32_path path)
{
	BYTE buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
	GIT_REPARSE_DATA_BUFFER *reparse_buf = (GIT_REPARSE_DATA_BUFFER *)buf;
	HANDLE handle = NULL;
	DWORD ioctl_ret;
	wchar_t *target;
	size_t target_len;

	int error = -1;

	handle = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		errno = ENOENT;
		return -1;
	}

	if (!DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, NULL, 0,
		reparse_buf, sizeof(buf), &ioctl_ret, NULL)) {
		errno = EINVAL;
		goto on_error;
	}

	switch (reparse_buf->ReparseTag) {
	case IO_REPARSE_TAG_SYMLINK:
		target = reparse_buf->SymbolicLinkReparseBuffer.PathBuffer +
			(reparse_buf->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(WCHAR));
		target_len = reparse_buf->SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
	break;
	case IO_REPARSE_TAG_MOUNT_POINT:
		target = reparse_buf->MountPointReparseBuffer.PathBuffer +
			(reparse_buf->MountPointReparseBuffer.SubstituteNameOffset / sizeof(WCHAR));
		target_len = reparse_buf->MountPointReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
	break;
	default:
		errno = EINVAL;
		goto on_error;
	}

	if (path_is_volume(target, target_len)) {
		/* This path is a reparse point that represents another volume mounted
		* at this location, it is not a symbolic link our input was canonical.
		*/
		errno = EINVAL;
		error = -1;
	} else if (target_len) {
		/* The path may need to have a prefix removed. */
		target_len = git_win32__canonicalize_path(target, target_len);

		/* Need one additional character in the target buffer
		* for the terminating NULL. */
		if (GIT_WIN_PATH_UTF16 > target_len) {
			wcscpy(dest, target);
			error = (int)target_len;
		}
	}

on_error:
	CloseHandle(handle);
	return error;
}
