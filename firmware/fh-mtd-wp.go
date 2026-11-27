// Toggle FiberHome board-driver MTD write protection via /dev/fhdrv_kdrv_board.
// At boot, fhdrv_kdrv_board_impl2.ko locks mtd0/1/2/3/6 so ubiupdatevol and
// bcm_bootstate return EPERM until mtd2 ("image") is temporarily unlocked.
//
// Usage: fh-mtd-wp <mtd_num> <0|1>   (0 = unlock, 1 = lock)
package main

import (
	"fmt"
	"os"
	"strconv"
	"syscall"
	"unsafe"
)

const ioctlFlashSet = 0x40085319 // fhdrv_kdrv_flash_set_ioctl: _IOW('S', 0x19, uint32[2])

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "usage: %s <mtd_num> <0|1>\n", os.Args[0])
		os.Exit(2)
	}
	mtd, err1 := strconv.ParseUint(os.Args[1], 10, 32)
	val, err2 := strconv.ParseUint(os.Args[2], 10, 32)
	if err1 != nil || err2 != nil || (val != 0 && val != 1) {
		fmt.Fprintln(os.Stderr, "invalid arguments")
		os.Exit(2)
	}
	fd, err := syscall.Open("/dev/fhdrv_kdrv_board", syscall.O_RDWR, 0)
	if err != nil {
		fmt.Fprintf(os.Stderr, "open /dev/fhdrv_kdrv_board: %v\n", err)
		os.Exit(1)
	}
	defer syscall.Close(fd)

	args := [2]uint32{uint32(mtd), uint32(val)}
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), uintptr(ioctlFlashSet), uintptr(unsafe.Pointer(&args[0])))
	if errno != 0 {
		fmt.Fprintf(os.Stderr, "ioctl 0x%x(%d, %d): %v\n", ioctlFlashSet, mtd, val, errno)
		os.Exit(1)
	}
}
