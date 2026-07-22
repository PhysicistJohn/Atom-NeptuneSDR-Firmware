/* SPDX-License-Identifier: MIT */
#ifndef NEPTUNE_TEST_LINUX_IOCTL_H
#define NEPTUNE_TEST_LINUX_IOCTL_H

/* Linux asm-generic ioctl encoding, sufficient for host-static ABI checks. */
#define _IOC_NRBITS      8U
#define _IOC_TYPEBITS    8U
#define _IOC_SIZEBITS    14U
#define _IOC_DIRBITS     2U

#define _IOC_NRSHIFT     0U
#define _IOC_TYPESHIFT   (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT   (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT    (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE        0U
#define _IOC_WRITE       1U
#define _IOC_READ        2U

#define _IOC(direction, type, number, size) \
	(((direction) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((number) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOC_SIZE(command) (((command) >> _IOC_SIZESHIFT) & \
			    ((1U << _IOC_SIZEBITS) - 1U))
#define _IOC_NR(command)   (((command) >> _IOC_NRSHIFT) & \
			    ((1U << _IOC_NRBITS) - 1U))

#define _IO(type, number)          _IOC(_IOC_NONE, (type), (number), 0U)
#define _IOR(type, number, value)  _IOC(_IOC_READ, (type), (number), sizeof(value))
#define _IOW(type, number, value)  _IOC(_IOC_WRITE, (type), (number), sizeof(value))

#endif
