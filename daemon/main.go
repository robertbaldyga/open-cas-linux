package main

// #include "cas_ioctl_codes.h"
import "C"
import (
	"fmt"
	"flag"
	"os"
	"syscall"
	"unsafe"
)

type Cas_ctrl struct {
	f *os.File
}

func Open(dev_path string) (ctrl *Cas_ctrl, err error) {
	f, err := os.OpenFile(dev_path, syscall.O_RDWR, 0666)
	if err != nil {
		return nil, err
	}
	return &Cas_ctrl{f: f}, nil
}

func (c *Cas_ctrl) Close() (err error) {
	return c.f.Close()
}

func (c *Cas_ctrl) StartCache() (err error) {
	return nil
}

func (c *Cas_ctrl) AddCore() (err error) {
	return nil
}

func ioctl(f *os.File, request uint, data interface{}) {
	_, _, errno := syscall.Syscall(
		syscall.SYS_IOCTL,
		uintptr(f.Fd()),
		uintptr(request),
		uintptr(unsafe.Pointer(data)),
	)
	return errno
}

func main() {
	debugMode := flag.Bool("debug", false, "Enable debugging")
	flag.Parse()

	if *debugMode {
		fmt.Println("Debugging!")
	}
	fmt.Println("KCAS_IOCTL_START_CACHE =", C.KCAS_IOCTL_START_CACHE)
}
