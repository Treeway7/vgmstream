#include "sf_utils.h"
#include "../vgmstream.h"
#include "reader_sf.h"
#include "paths.h"


/* change pathname's extension to another (or add it if extensionless) */
static void swap_extension(char* pathname, /*size_t*/ int pathname_len, const char* swap) {
    char* extension = (char*)filename_extension(pathname);
    //todo safeops
    if (extension[0] == '\0') {
        strcat(pathname, ".");
        strcat(pathname, swap);
    }
    else {
        strcpy(extension, swap);
    }
}

STREAMFILE* open_streamfile_by_ext(STREAMFILE* sf, const char* ext) {
    char filename[PATH_LIMIT];

    get_streamfile_name(sf, filename, sizeof(filename));

    swap_extension(filename, sizeof(filename), ext);

    return open_streamfile(sf, filename);
}

STREAMFILE* open_streamfile_by_filename(STREAMFILE* sf, const char* filename) {
    char fullname[PATH_LIMIT];
    char partname[PATH_LIMIT];
    char *path, *name, *otherpath;

    if (!sf || !filename || !filename[0]) return NULL;

    get_streamfile_name(sf, fullname, sizeof(fullname));

    //todo normalize separators in a better way, safeops, improve copying

    /* check for non-normalized paths first (ex. txth) */
    path = strrchr(fullname, '/');
    otherpath = strrchr(fullname, '\\');
    if (otherpath > path) { //todo cast to ptr?
        /* foobar makes paths like "(fake protocol)://(windows path with \)".
         * Hack to work around both separators, though probably foo_streamfile
         * should just return and handle normalized paths without protocol. */
        path = otherpath;
    }

    if (path) {
        path[1] = '\0'; /* remove name after separator */

        strcpy(partname, filename);
        fix_dir_separators(partname); /* normalize to DIR_SEPARATOR */

        /* normalize relative paths as don't work ok in some plugins */
        if (partname[0] == '.' && partname[1] == DIR_SEPARATOR) { /* './name' */
            name = partname + 2; /* ignore './' */
        }
        else if (partname[0] == '.' && partname[1] == '.' && partname[2] == DIR_SEPARATOR) { /* '../name' */
            char* pathprev;

            path[0] = '\0'; /* remove last separator so next call works */
            pathprev = strrchr(fullname,DIR_SEPARATOR);
            if (pathprev) {
                pathprev[1] = '\0'; /* remove prev dir after separator */
                name = partname + 3; /* ignore '../' */
            }
            else { /* let plugin handle? */
                path[0] = DIR_SEPARATOR;
                name = partname;
            }
            /* could work with more relative paths but whatevs */
        }
        else {
            name = partname;
        }

        strcat(fullname, name);
    }
    else {
        strcpy(fullname, filename);
    }

    return open_streamfile(sf, fullname);
}

/* ************************************************************************* */

int check_extensions(STREAMFILE* sf, const char* cmp_exts) {
    char filename[PATH_LIMIT];
    const char* ext = NULL;
    const char* cmp_ext = NULL;
    const char* ststr_res = NULL;
    size_t ext_len, cmp_len;

    sf->get_name(sf, filename, sizeof(filename));
    ext = filename_extension(filename);
    ext_len = strlen(ext);

    cmp_ext = cmp_exts;
    do {
        ststr_res = strstr(cmp_ext, ",");
        cmp_len = ststr_res == NULL
                  ? strlen(cmp_ext) /* total length if more not found */
                  : (intptr_t)ststr_res - (intptr_t)cmp_ext; /* find next ext; ststr_res should always be greater than cmp_ext, resulting in a positive cmp_len */

        if (ext_len == cmp_len && strncasecmp(ext,cmp_ext, ext_len) == 0)
            return 1;

        cmp_ext = ststr_res;
        if (cmp_ext != NULL)
            cmp_ext = cmp_ext + 1; /* skip comma */

    } while (cmp_ext != NULL);

    return 0;
}

/* ************************************************************************* */

/* copies name as-is (may include full path included) */
void get_streamfile_name(STREAMFILE* sf, char* buffer, size_t size) {
    sf->get_name(sf, buffer, size);
}

/* copies the filename without path */
void get_streamfile_filename(STREAMFILE* sf, char* buffer, size_t size) {
    char foldername[PATH_LIMIT];
    const char* path;


    get_streamfile_name(sf, foldername, sizeof(foldername));

    //todo Windows CMD accepts both \\ and /, better way to handle this?
    path = strrchr(foldername,'\\');
    if (!path)
        path = strrchr(foldername,'/');
    if (path != NULL)
        path = path+1;

    //todo validate sizes and copy sensible max
    if (path) {
        strcpy(buffer, path);
    } else {
        strcpy(buffer, foldername);
    }
}

/* copies the filename without path or extension */
void get_streamfile_basename(STREAMFILE* sf, char* buffer, size_t size) {
    char* ext;

    get_streamfile_filename(sf, buffer, size);

    ext = strrchr(buffer,'.');
    if (ext) {
        ext[0] = '\0'; /* remove .ext from buffer */
    }
}

/* copies path removing name (NULL when if filename has no path) */
void get_streamfile_path(STREAMFILE* sf, char* buffer, size_t size) {
    const char* path;

    get_streamfile_name(sf, buffer, size);

    path = strrchr(buffer,DIR_SEPARATOR);
    if (path!=NULL) path = path+1; /* includes "/" */

    if (path) {
        buffer[path - buffer] = '\0';
    } else {
        buffer[0] = '\0';
    }
}

/* copies extension only */
void get_streamfile_ext(STREAMFILE* sf, char* buffer, size_t size) {
    char filename[PATH_LIMIT];
    const char* extension = NULL;

    get_streamfile_name(sf, filename, sizeof(filename));
    extension = filename_extension(filename);
    if (!extension) {
        buffer[0] = '\n';
    }
    else {
        strncpy(buffer, extension, size); //todo use something better
    }
}
