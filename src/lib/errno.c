#include "lib/errno.h"

const char *strerror(int err) {
    switch (err < 0 ? -err : err) {
    case 0:            return "success";
    case EPERM:        return "operation not permitted";
    case ENOENT:       return "no such file or directory";
    case EINTR:        return "interrupted";
    case EIO:          return "I/O error";
    case ENOEXEC:      return "not an executable";
    case EBADF:        return "bad file descriptor";
    case ECHILD:       return "no child processes";
    case ENOMEM:       return "out of memory";
    case EACCES:       return "permission denied";
    case EFAULT:       return "bad address";
    case EBUSY:        return "resource busy";
    case EEXIST:       return "file exists";
    case EXDEV:        return "can't move between drives";
    case EROFS:        return "the drive is read-only";
    case ENODEV:       return "no such device";
    case ENOTDIR:      return "not a directory";
    case EISDIR:       return "is a directory";
    case EINVAL:       return "invalid argument";
    case EMFILE:       return "too many open files";
    case ENOSPC:       return "no space left";
    case ENAMETOOLONG: return "name too long";
    case ENOSYS:       return "function not implemented";
    case ENOTEMPTY:    return "directory not empty";
    case EAGAIN:       return "try again";
    case EPIPE:        return "the other end is closed";
    case EMSGSIZE:     return "message too long";
    case ECONNREFUSED: return "connection refused";
    case ENFILE:       return "too many open files in the system";
    case ERANGE:       return "out of range";
    case ENOTSOCK:     return "not a socket";
    case EDESTADDRREQ: return "destination address required";
    case EPROTOTYPE:   return "wrong protocol for this socket";
    case ENOPROTOOPT:  return "protocol option not available";
    case EPROTONOSUPPORT: return "protocol not supported";
    case EOPNOTSUPP:   return "operation not supported";
    case EAFNOSUPPORT: return "address family not supported";
    case EADDRINUSE:   return "address already in use";
    case EADDRNOTAVAIL: return "address not available";
    case ENETDOWN:     return "network is down";
    case ENETUNREACH:  return "network is unreachable";
    case ECONNABORTED: return "connection aborted";
    case ECONNRESET:   return "connection reset by peer";
    case ENOBUFS:      return "no buffer space available";
    case EISCONN:      return "already connected";
    case ENOTCONN:     return "not connected";
    case ESHUTDOWN:    return "connection shut down";
    case ETIMEDOUT:    return "timed out";
    case EHOSTDOWN:    return "host is down";
    case EHOSTUNREACH: return "no route to host";
    case EALREADY:     return "already in progress";
    case EINPROGRESS:  return "in progress";
    default:           return "unknown error";
    }
}
