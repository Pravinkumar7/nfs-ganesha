/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * -------------
 */

/**
 *
 * \file    fsal_internal.c
 * \author  $Author: leibovic $
 * \date    $Date: 2006/01/17 14:20:07 $
 * \version $Revision: 1.24 $
 * \brief   Defines the datas that are to be
 *          accessed as extern by the fsal modules
 *
 */
#define FSAL_INTERNAL_C
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/ioctl.h>
#include  "fsal.h"
#include "fsal_internal.h"
#include "stuff_alloc.h"
#include "SemN.h"
#include "fsal_convert.h"
#include <libgen.h>             /* used for 'dirname' */

#include <pthread.h>
#include <string.h>

/* credential lifetime (1h) */
fsal_uint_t CredentialLifetime = 3600;

/* static filesystem info.
 * The access is thread-safe because
 * it is read-only, except during initialization.
 */
fsal_staticfsinfo_t global_fs_info;

char open_by_handle_path[MAXPATHLEN];
int open_by_handle_fd;

/* filesystem info for HPSS */
static fsal_staticfsinfo_t default_vfs_info = {
  0xFFFFFFFFFFFFFFFFLL,         /* max file size (64bits) */
  _POSIX_LINK_MAX,              /* max links */
  FSAL_MAX_NAME_LEN,            /* max filename */
  FSAL_MAX_PATH_LEN,            /* max pathlen */
  TRUE,                         /* no_trunc */
  TRUE,                         /* chown restricted */
  FALSE,                        /* case insensitivity */
  TRUE,                         /* case preserving */
  FSAL_EXPTYPE_PERSISTENT,      /* FH expire type */
  TRUE,                         /* hard link support */
  TRUE,                         /* symlink support */
  FALSE,                        /* lock management */
  TRUE,                         /* named attributes */
  TRUE,                         /* handles are unique and persistent */
  {10, 0},                      /* Duration of lease at FS in seconds */
  FSAL_ACLSUPPORT_ALLOW,        /* ACL support */
  TRUE,                         /* can change times */
  TRUE,                         /* homogenous */
  VFS_SUPPORTED_ATTRIBUTES,    /* supported attributes */
  0,                            /* maxread size */
  0,                            /* maxwrite size */
  0,                            /* default umask */
  0,                            /* cross junctions */
  0400                          /* default access rights for xattrs: root=RW, owner=R */
};

/* variables for limiting the calls to the filesystem */
static int limit_calls = FALSE;
semaphore_t sem_fs_calls;

/* threads keys for stats */
static pthread_key_t key_stats;
static pthread_once_t once_key = PTHREAD_ONCE_INIT;

static void free_pthread_specific_stats(void *buff)
{
  Mem_Free(buff);
}

/* init keys */
static void init_keys(void)
{
  if(pthread_key_create(&key_stats, free_pthread_specific_stats) == -1)
    LogMajor(COMPONENT_FSAL,
             "Could not create thread specific stats (pthread_key_create) err %d (%s)",
             errno, strerror(errno));

  return;
}                               /* init_keys */

/**
 * fsal_increment_nbcall:
 * Updates fonction call statistics.
 *
 * \param function_index (input):
 *        Index of the function whom number of call is to be incremented.
 * \param status (input):
 *        Status the function returned.
 *
 * \return Nothing.
 */
void fsal_increment_nbcall(int function_index, fsal_status_t status)
{

  fsal_statistics_t *bythread_stat = NULL;

  /* verify index */

  if(function_index >= FSAL_NB_FUNC)
    return;

  /* first, we init the keys if this is the first time */

  if(pthread_once(&once_key, init_keys) != 0)
    {
      LogMajor(COMPONENT_FSAL,
               "Could not create thread specific stats (pthread_once) err %d (%s)",
               errno, strerror(errno));
      return;
    }

  /* we get the specific value */

  bythread_stat = (fsal_statistics_t *) pthread_getspecific(key_stats);

  /* we allocate stats if this is the first time */

  if(bythread_stat == NULL)
    {
      int i;

      bythread_stat = (fsal_statistics_t *) Mem_Alloc_Label(sizeof(fsal_statistics_t), "fsal_statistics_t");

      if(bythread_stat == NULL)
        {
          LogCrit(COMPONENT_FSAL,
                  "Could not allocate memory for FSAL statistics err %d (%s)",
                  Mem_Errno, strerror(Mem_Errno));
          /* we don't have real memory, bail */
          return;
        }

      /* inits the struct */

      for(i = 0; i < FSAL_NB_FUNC; i++)
        {
          bythread_stat->func_stats.nb_call[i] = 0;
          bythread_stat->func_stats.nb_success[i] = 0;
          bythread_stat->func_stats.nb_err_retryable[i] = 0;
          bythread_stat->func_stats.nb_err_unrecover[i] = 0;
        }

      /* set the specific value */
      pthread_setspecific(key_stats, (void *)bythread_stat);

    }

  /* we increment the values */

  if(bythread_stat)
    {
      bythread_stat->func_stats.nb_call[function_index]++;

      if(!FSAL_IS_ERROR(status))
        bythread_stat->func_stats.nb_success[function_index]++;
      else if(fsal_is_retryable(status))
        bythread_stat->func_stats.nb_err_retryable[function_index]++;
      else
        bythread_stat->func_stats.nb_err_unrecover[function_index]++;
    }

  return;
}

/**
 * fsal_internal_getstats:
 * (For internal use in the FSAL).
 * Retrieve call statistics for current thread.
 *
 * \param output_stats (output):
 *        Pointer to the call statistics structure.
 *
 * \return Nothing.
 */
void fsal_internal_getstats(fsal_statistics_t * output_stats)
{

  fsal_statistics_t *bythread_stat = NULL;

  /* first, we init the keys if this is the first time */
  if(pthread_once(&once_key, init_keys) != 0)
    {
      LogMajor(COMPONENT_FSAL,
               "Could not create thread specific stats (pthread_once) err %d (%s)",
               errno, strerror(errno));
      return;
    }

  /* we get the specific value */
  bythread_stat = (fsal_statistics_t *) pthread_getspecific(key_stats);

  /* we allocate stats if this is the first time */
  if(bythread_stat == NULL)
    {
      int i;

      if((bythread_stat =
          (fsal_statistics_t *) Mem_Alloc_Label(sizeof(fsal_statistics_t), "fsal_statistics_t")) == NULL)
      {
        /* we don't have working memory, bail */
        LogCrit(COMPONENT_FSAL,
                "Could not allocate memory for FSAL statistics err %d (%s)",
                Mem_Errno, strerror(Mem_Errno));
        return;
      }

      /* inits the struct */
      for(i = 0; i < FSAL_NB_FUNC; i++)
        {
          bythread_stat->func_stats.nb_call[i] = 0;
          bythread_stat->func_stats.nb_success[i] = 0;
          bythread_stat->func_stats.nb_err_retryable[i] = 0;
          bythread_stat->func_stats.nb_err_unrecover[i] = 0;
        }

      /* set the specific value */
      pthread_setspecific(key_stats, (void *)bythread_stat);

    }

  if(output_stats)
    (*output_stats) = (*bythread_stat);

  return;

}

/**
 * Set credential lifetime.
 * (For internal use in the FSAL).
 * Set the period for thread's credential renewal.
 *
 * \param lifetime_in (input):
 *        The period for thread's credential renewal.
 *
 * \return Nothing.
 */
void fsal_internal_SetCredentialLifetime(fsal_uint_t lifetime_in)
{
  CredentialLifetime = lifetime_in;
}

/**
 *  Used to limit the number of simultaneous calls to Filesystem.
 */
void TakeTokenFSCall()
{
  /* no limits */
  if(limit_calls == FALSE)
    return;

  /* there is a limit */
  semaphore_P(&sem_fs_calls);

}

void ReleaseTokenFSCall()
{
  /* no limits */
  if(limit_calls == FALSE)
    return;

  /* there is a limit */
  semaphore_V(&sem_fs_calls);

}

#define SET_INTEGER_PARAM( cfg, p_init_info, _field )             \
    switch( (p_init_info)->behaviors._field ){                    \
    case FSAL_INIT_FORCE_VALUE :                                  \
      /* force the value in any case */                           \
      cfg._field = (p_init_info)->values._field;                  \
      break;                                                      \
    case FSAL_INIT_MAX_LIMIT :                                    \
      /* check the higher limit */                                \
      if ( cfg._field > (p_init_info)->values._field )            \
        cfg._field = (p_init_info)->values._field ;               \
      break;                                                      \
    case FSAL_INIT_MIN_LIMIT :                                    \
      /* check the lower limit */                                 \
      if ( cfg._field < (p_init_info)->values._field )            \
        cfg._field = (p_init_info)->values._field ;               \
      break;                                                      \
    case FSAL_INIT_FS_DEFAULT:                                    \
    default:                                                      \
    /* In the other cases, we keep the default value. */          \
        break;                                                    \
    }

#define SET_BITMAP_PARAM( cfg, p_init_info, _field )              \
    switch( (p_init_info)->behaviors._field ){                    \
    case FSAL_INIT_FORCE_VALUE :                                  \
        /* force the value in any case */                         \
        cfg._field = (p_init_info)->values._field;                \
        break;                                                    \
    case FSAL_INIT_MAX_LIMIT :                                    \
      /* proceed a bit AND */                                     \
      cfg._field &= (p_init_info)->values._field ;                \
      break;                                                      \
    case FSAL_INIT_MIN_LIMIT :                                    \
      /* proceed a bit OR */                                      \
      cfg._field |= (p_init_info)->values._field ;                \
      break;                                                      \
    case FSAL_INIT_FS_DEFAULT:                                    \
    default:                                                      \
    /* In the other cases, we keep the default value. */          \
        break;                                                    \
    }

#define SET_BOOLEAN_PARAM( cfg, p_init_info, _field )             \
    switch( (p_init_info)->behaviors._field ){                    \
    case FSAL_INIT_FORCE_VALUE :                                  \
        /* force the value in any case */                         \
        cfg._field = (p_init_info)->values._field;                \
        break;                                                    \
    case FSAL_INIT_MAX_LIMIT :                                    \
      /* proceed a boolean AND */                                 \
      cfg._field = cfg._field && (p_init_info)->values._field ;   \
      break;                                                      \
    case FSAL_INIT_MIN_LIMIT :                                    \
      /* proceed a boolean OR */                                  \
      cfg._field = cfg._field && (p_init_info)->values._field ;   \
      break;                                                      \
    case FSAL_INIT_FS_DEFAULT:                                    \
    default:                                                      \
    /* In the other cases, we keep the default value. */          \
        break;                                                    \
    }

/*
 *  This function initializes shared variables of the fsal.
 */
fsal_status_t fsal_internal_init_global(fsal_init_info_t * fsal_info,
                                        fs_common_initinfo_t * fs_common_info,
                                        fs_specific_initinfo_t * fs_specific_info)
{

  /* sanity check */
  if(!fsal_info || !fs_common_info || !fs_specific_info)
    ReturnCode(ERR_FSAL_FAULT, 0);

  /* inits FS call semaphore */
  if(fsal_info->max_fs_calls > 0)
    {
      int rc;

      limit_calls = TRUE;

      rc = semaphore_init(&sem_fs_calls, fsal_info->max_fs_calls);

      if(rc != 0)
        ReturnCode(ERR_FSAL_SERVERFAULT, rc);

      LogDebug(COMPONENT_FSAL,
               "FSAL INIT: Max simultaneous calls to filesystem is limited to %u.",
               fsal_info->max_fs_calls);
    }
  else
    {
      LogDebug(COMPONENT_FSAL,
               "FSAL INIT: Max simultaneous calls to filesystem is unlimited.");
    }

  /* setting default values. */
  global_fs_info = default_vfs_info;

  if(isFullDebug(COMPONENT_FSAL))
    {
      LogFullDebug(COMPONENT_FSAL, "{");
      LogFullDebug(COMPONENT_FSAL,
                   "  maxfilesize  = %llX    ",
                   default_vfs_info.maxfilesize);
      LogFullDebug(COMPONENT_FSAL,
                   "  maxlink  = %lu   ",
                   default_vfs_info.maxlink);
      LogFullDebug(COMPONENT_FSAL,
                   "  maxnamelen  = %lu  ",
                   default_vfs_info.maxnamelen);
      LogFullDebug(COMPONENT_FSAL,
                   "  maxpathlen  = %lu  ",
                   default_vfs_info.maxpathlen);
      LogFullDebug(COMPONENT_FSAL,
                   "  no_trunc  = %d ",
                   default_vfs_info.no_trunc);
      LogFullDebug(COMPONENT_FSAL,
                   "  chown_restricted  = %d ",
                   default_vfs_info.chown_restricted);
      LogFullDebug(COMPONENT_FSAL,
                   "  case_insensitive  = %d ",
                   default_vfs_info.case_insensitive);
      LogFullDebug(COMPONENT_FSAL,
                   "  case_preserving  = %d ",
                   default_vfs_info.case_preserving);
      LogFullDebug(COMPONENT_FSAL,
                   "  fh_expire_type  = %hu ",
                   default_vfs_info.fh_expire_type);
      LogFullDebug(COMPONENT_FSAL,
                   "  link_support  = %d  ",
                   default_vfs_info.link_support);
      LogFullDebug(COMPONENT_FSAL,
                   "  symlink_support  = %d  ",
                   default_vfs_info.symlink_support);
      LogFullDebug(COMPONENT_FSAL,
                   "  lock_support  = %d  ",
                   default_vfs_info.lock_support);
      LogFullDebug(COMPONENT_FSAL,
                   "  named_attr  = %d  ",
                   default_vfs_info.named_attr);
      LogFullDebug(COMPONENT_FSAL,
                   "  unique_handles  = %d  ",
                   default_vfs_info.unique_handles);
      LogFullDebug(COMPONENT_FSAL,
                   "  acl_support  = %hu  ",
                   default_vfs_info.acl_support);
      LogFullDebug(COMPONENT_FSAL,
                   "  cansettime  = %d  ",
                   default_vfs_info.cansettime);
      LogFullDebug(COMPONENT_FSAL,
                   "  homogenous  = %d  ",
                   default_vfs_info.homogenous);
      LogFullDebug(COMPONENT_FSAL,
                   "  supported_attrs  = %llX  ",
                   default_vfs_info.supported_attrs);
      LogFullDebug(COMPONENT_FSAL,
                   "  maxread  = %llX     ",
                   default_vfs_info.maxread);
      LogFullDebug(COMPONENT_FSAL,
                   "  maxwrite  = %llX     ",
                   default_vfs_info.maxwrite);
      LogFullDebug(COMPONENT_FSAL,
                   "  umask  = %X ",
                   default_vfs_info.umask);
      LogFullDebug(COMPONENT_FSAL, "}");
    }

  /* Analyzing fs_common_info struct */

  if((fs_common_info->behaviors.maxfilesize != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.maxlink != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.maxnamelen != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.maxpathlen != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.no_trunc != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.case_insensitive != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.case_preserving != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.named_attr != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.lease_time != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.supported_attrs != FSAL_INIT_FS_DEFAULT) ||
     (fs_common_info->behaviors.homogenous != FSAL_INIT_FS_DEFAULT))
    ReturnCode(ERR_FSAL_NOTSUPP, 0);

  SET_BOOLEAN_PARAM(global_fs_info, fs_common_info, symlink_support);
  SET_BOOLEAN_PARAM(global_fs_info, fs_common_info, link_support);
  SET_BOOLEAN_PARAM(global_fs_info, fs_common_info, lock_support);
  SET_BOOLEAN_PARAM(global_fs_info, fs_common_info, cansettime);

  SET_INTEGER_PARAM(global_fs_info, fs_common_info, maxread);
  SET_INTEGER_PARAM(global_fs_info, fs_common_info, maxwrite);

  SET_BITMAP_PARAM(global_fs_info, fs_common_info, umask);

  SET_BOOLEAN_PARAM(global_fs_info, fs_common_info, auth_exportpath_xdev);

  SET_BITMAP_PARAM(global_fs_info, fs_common_info, xattr_access_rights);

  LogFullDebug(COMPONENT_FSAL,
               "Supported attributes constant = 0x%llX.",
               VFS_SUPPORTED_ATTRIBUTES);

  LogFullDebug(COMPONENT_FSAL,
               "Supported attributes default = 0x%llX.",
               default_vfs_info.supported_attrs);

  LogFullDebug(COMPONENT_FSAL,
               "FSAL INIT: Supported attributes mask = 0x%llX.",
               global_fs_info.supported_attrs);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

/*********************************************************************
 *
 *  VFS FSAL char device driver interaces
 *
 ********************************************************************/

/**
 * fsal_internal_handle2fd:
 * Open a file by handle within an export.
 *
 * \param p_context (input):
 *        Pointer to current context.  Used to get export root fd.
 * \param phandle (input):
 *        Opaque filehandle
 * \param pfd (output):
 *        File descriptor openned by the function
 * \param oflags (input):
 *        Flags to open the file with
 *
 * \return status of operation
 */

fsal_status_t fsal_internal_handle2fd(fsal_op_context_t * p_context,
                                      fsal_handle_t * phandle, int *pfd, int oflags)
{
  int rc = 0;
  int dirfd = 0;
  fsal_status_t status;

  if(!phandle || !pfd || !p_context || !p_context->export_context)
    ReturnCode(ERR_FSAL_FAULT, 0);

  dirfd = p_context->export_context->mount_root_fd;

  status = fsal_internal_handle2fd_at(dirfd, phandle, pfd, oflags);

  if(FSAL_IS_ERROR(status))
    ReturnStatus(status, INDEX_FSAL_open);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

/**
 * fsal_internal_handle2fd_at:
 * Open a file by handle from in an open directory
 *
 * \param dirfd (input):
 *        Open file descriptor of parent directory
 * \param phandle (input):
 *        Opaque filehandle
 * \param pfd (output):
 *        File descriptor openned by the function
 * \param oflags (input):
 *        Flags to open the file with
 *
 * \return status of operation
 */

fsal_status_t fsal_internal_handle2fd_at(int dirfd,
                                         fsal_handle_t * phandle, int *pfd, int oflags)
{
  int rc = 0;
  struct open_arg oarg;

  if(!phandle || !pfd)
    ReturnCode(ERR_FSAL_FAULT, 0);

  oarg.mountdirfd = dirfd;

  oarg.handle = &phandle->data.handle;
  oarg.flags = oflags;

  if((rc = ioctl(open_by_handle_fd, OPENHANDLE_OPEN_BY_HANDLE, &oarg)) < 0)
    ReturnCode(posix2fsal_error(errno), errno);

  *pfd = rc;

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

/**
 * fsal_internal_get_handle:
 * Create a handle from a file path
 *
 * \param pcontext (input):
 *        A context pointer for the root of the current export
 * \param p_fsalpath (input):
 *        Full path to the file
 * \param p_handle (output):
 *        The handle that is found and returned
 *
 * \return status of operation
 */
fsal_status_t fsal_internal_get_handle(fsal_op_context_t * p_context,   /* IN */
                                       fsal_path_t * p_fsalpath,        /* IN */
                                       fsal_handle_t * p_handle /* OUT */ )
{
  int rc;
  struct name_handle_arg harg;
  int objectfd;

  if(!p_context || !p_handle || !p_fsalpath)
    ReturnCode(ERR_FSAL_FAULT, 0);

  harg.handle = &p_handle->data.handle;
  harg.handle->handle_size = OPENHANDLE_HANDLE_LEN;
  harg.name = p_fsalpath->path;
  harg.dfd = AT_FDCWD;
  harg.flag = 0;

  LogFullDebug(COMPONENT_FSAL,
               "Lookup handle for %s",
               p_fsalpath->path);

  if((rc = ioctl(open_by_handle_fd, OPENHANDLE_NAME_TO_HANDLE, &harg)) < 0)
    ReturnCode(posix2fsal_error(errno), errno);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

/**
 * fsal_internal_get_handle_at:
 * Create a handle from a directory pointer and filename
 *
 * \param dfd (input):
 *        Open directory handle
 * \param p_fsalname (input):
 *        Name of the file
 * \param p_handle (output):
 *        The handle that is found and returned
 *
 * \return status of operation
 */

fsal_status_t fsal_internal_get_handle_at(int dfd,      /* IN */
                                          fsal_name_t * p_fsalname,     /* IN */
                                          fsal_handle_t * p_handle      /* OUT
                                                                         */ )
{
  int rc;
  struct name_handle_arg harg;
  int objectfd;

  if(!p_handle || !p_fsalname)
    ReturnCode(ERR_FSAL_FAULT, 0);

  harg.handle = &p_handle->data.handle;
  harg.handle->handle_size = OPENHANDLE_HANDLE_LEN;
  harg.name = p_fsalname->name;
  harg.dfd = dfd;
  harg.flag = 0;

  LogFullDebug(COMPONENT_FSAL,
               "Lookup handle at for %s",
               p_fsalname->name);

  if((rc = ioctl(open_by_handle_fd, OPENHANDLE_NAME_TO_HANDLE, &harg)) < 0)
    ReturnCode(posix2fsal_error(errno), errno);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

/**
 * fsal_internal_fd2handle:
 * convert an fd to a handle
 *
 * \param fd (input):
 *        Open file descriptor for target file
 * \param p_handle (output):
 *        The handle that is found and returned
 *
 * \return status of operation
 */
fsal_status_t fsal_internal_fd2handle(int fd, fsal_handle_t * p_handle)
{
  int rc;
  struct name_handle_arg harg;

  if(!p_handle || !&p_handle->data.handle)
    ReturnCode(ERR_FSAL_FAULT, 0);

  harg.handle = &p_handle->data.handle;
  memset(&p_handle->data.handle, 0, sizeof(struct file_handle));

  harg.handle->handle_size = OPENHANDLE_HANDLE_LEN;
  harg.name = NULL;
  harg.dfd = fd;
  harg.flag = 0;

  LogFullDebug(COMPONENT_FSAL,
               "Lookup handle by fd for %d",
               fd);

  if((rc = ioctl(open_by_handle_fd, OPENHANDLE_NAME_TO_HANDLE, &harg)) < 0)
    ReturnCode(posix2fsal_error(errno), errno);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

/**
 * fsal_internal_link_at:
 * Create a link based on a file descriptor, dirfd, and new name
 *
 * \param srcfd (input):
 *          file descriptor of source file
 * \param dirfd (input):
 *          file descriptor of target directory
 * \param name (input):
 *          name for the new file
 *
 * \return status of operation
 */
fsal_status_t fsal_internal_link_at(int srcfd, int dirfd, char *name)
{
  int rc;
  struct link_arg linkarg;

  if(!name)
    ReturnCode(ERR_FSAL_FAULT, 0);

  linkarg.dir_fd = dirfd;
  linkarg.file_fd = srcfd;
  linkarg.name = name;

  if((rc = ioctl(open_by_handle_fd, OPENHANDLE_LINK_BY_FD, &linkarg)) < 0)
    ReturnCode(posix2fsal_error(errno), errno);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);

}

/**
 * fsal_readlink_by_handle:
 * Reads the contents of the link
 *
 *
 * \return status of operation
 */

fsal_status_t fsal_readlink_by_handle(fsal_op_context_t * p_context,
                                      fsal_handle_t * p_handle, char *__buf, int maxlen)
{
  int fd;
  int rc;
  fsal_status_t status;
  struct readlink_arg readlinkarg;

  status = fsal_internal_handle2fd(p_context, p_handle, &fd, O_RDONLY);

  if(FSAL_IS_ERROR(status))
    ReturnStatus(status, INDEX_FSAL_open);

  memset(__buf, 0, maxlen);
  readlinkarg.fd = fd;
  readlinkarg.buffer = __buf;
  readlinkarg.size = maxlen;

  rc = ioctl(open_by_handle_fd, OPENHANDLE_READLINK_BY_FD, &readlinkarg);

  close(fd);

  if(rc < 0)
      Return(rc, 0, INDEX_FSAL_readlink);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}

/*
   Check the access from an existing fsal_attrib_list_t or struct stat
*/
/* XXX : ACL */
fsal_status_t fsal_internal_testAccess(fsal_op_context_t * p_context,   /* IN */
                                       fsal_accessflags_t access_type,  /* IN */
                                       struct stat *p_buffstat, /* IN */
                                       fsal_attrib_list_t * p_object_attributes /* IN */ )
{
  fsal_accessflags_t missing_access;
  unsigned int is_grp, i;
  fsal_uid_t uid;
  fsal_gid_t gid;
  fsal_accessmode_t mode;

  /* sanity checks. */

  if((!p_object_attributes && !p_buffstat) || !p_context)
    ReturnCode(ERR_FSAL_FAULT, 0);

  /* If the FSAL_F_OK flag is set, returns ERR INVAL */

  if(access_type & FSAL_F_OK)
    ReturnCode(ERR_FSAL_INVAL, 0);

  /* The root user ignores the mode/uid/gid of the file */
  if(p_context->credential.user == 0)
    ReturnCode(ERR_FSAL_NO_ERROR, 0);

  /* unsatisfied flags */
  missing_access = access_type;

  if(p_object_attributes)
    {
      uid = p_object_attributes->owner;
      gid = p_object_attributes->group;
      mode = p_object_attributes->mode;

    }
  else
    {
      uid = p_buffstat->st_uid;
      gid = p_buffstat->st_gid;
      mode = unix2fsal_mode(p_buffstat->st_mode);
    }

  LogFullDebug(COMPONENT_FSAL,
               "file Mode=%#o, file uid=%d, file gid= %d",
               mode,uid, gid);
  LogFullDebug(COMPONENT_FSAL,
               "user uid=%d, user gid= %d, access_type=%#o",
               p_context->credential.user,
               p_context->credential.group,
               access_type);

  /* If the uid of the file matches the uid of the user,
   * then the uid mode bits take precedence. */
  if(p_context->credential.user == uid)
    {

      LogFullDebug(COMPONENT_FSAL,
                   "File belongs to user %d", uid);

      if(mode & FSAL_MODE_RUSR)
        missing_access &= ~FSAL_R_OK;

      if(mode & FSAL_MODE_WUSR)
        missing_access &= ~FSAL_W_OK;

      if(mode & FSAL_MODE_XUSR)
        missing_access &= ~FSAL_X_OK;

      /* handle the creation of a new 500 file correctly */
      if((missing_access & FSAL_OWNER_OK) != 0)
        missing_access = 0;

      if(missing_access == 0)
        ReturnCode(ERR_FSAL_NO_ERROR, 0);
      else
        {
          LogFullDebug(COMPONENT_FSAL,
                       "Mode=%#o, Access=%#o, Rights missing: %#o",
                       mode, access_type, missing_access);
          ReturnCode(ERR_FSAL_ACCESS, 0);
        }

    }

  /* missing_access will be nonzero triggering a failure
   * even though FSAL_OWNER_OK is not even a real posix file
   * permission */
  missing_access &= ~FSAL_OWNER_OK;

  /* Test if the file belongs to user's group. */
  is_grp = (p_context->credential.group == gid);
  if(is_grp)
    LogFullDebug(COMPONENT_FSAL,
                 "File belongs to user's group %d",
                 p_context->credential.group);

  /* Test if file belongs to alt user's groups */
  if(!is_grp)
    for(i = 0; i < p_context->credential.nbgroups; i++)
      {
        is_grp = (p_context->credential.alt_groups[i] == gid);
        if(is_grp)
          LogFullDebug(COMPONENT_FSAL,
                       "File belongs to user's alt group %d",
                       p_context->credential.alt_groups[i]);
        if(is_grp)
          break;
      }

  /* If the gid of the file matches the gid of the user or
   * one of the alternatve gids of the user, then the uid mode
   * bits take precedence. */
  if(is_grp)
    {
      if(mode & FSAL_MODE_RGRP)
        missing_access &= ~FSAL_R_OK;

      if(mode & FSAL_MODE_WGRP)
        missing_access &= ~FSAL_W_OK;

      if(mode & FSAL_MODE_XGRP)
        missing_access &= ~FSAL_X_OK;

      if(missing_access == 0)
        ReturnCode(ERR_FSAL_NO_ERROR, 0);
      else
        ReturnCode(ERR_FSAL_ACCESS, 0);

    }

  /* If the user uid is not 0, the uid does not match the file's, and
   * the user's gids do not match the file's gid, we apply the "other"
   * mode bits to the user. */
  if(mode & FSAL_MODE_ROTH)
    missing_access &= ~FSAL_R_OK;

  if(mode & FSAL_MODE_WOTH)
    missing_access &= ~FSAL_W_OK;

  if(mode & FSAL_MODE_XOTH)
    missing_access &= ~FSAL_X_OK;

  if(missing_access == 0)
    ReturnCode(ERR_FSAL_NO_ERROR, 0);
  else {
    LogFullDebug(COMPONENT_FSAL,
                 "Mode=%#o, Access=%#o, Rights missing: %#o",
                 mode, access_type, missing_access);
    ReturnCode(ERR_FSAL_ACCESS, 0);
  }

}

/**
 * fsal_stat_by_handle:
 * get the stat value
 *
 *
 * \return status of operation
 */

fsal_status_t fsal_stat_by_handle(fsal_op_context_t * p_context,
                                  fsal_handle_t * p_handle, struct stat64 *buf)
{
  int rc;
  int dirfd = 0;
  struct stat_arg statarg;

  if(!p_handle || !p_context || !p_context->export_context)
      ReturnCode(ERR_FSAL_FAULT, 0);

  dirfd = p_context->export_context->mount_root_fd;

  statarg.mountdirfd = dirfd;

  statarg.handle = &p_handle->data.handle;
  statarg.buf = buf;

  if((rc = ioctl(open_by_handle_fd, OPENHANDLE_STAT_BY_HANDLE, &statarg)) < 0)
      ReturnCode(posix2fsal_error(errno), errno);

  ReturnCode(ERR_FSAL_NO_ERROR, 0);
}