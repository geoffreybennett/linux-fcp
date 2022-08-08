#!/bin/bash
# v1.00
# To execute: `chmod +x ./check_version.sh` then `./check_version.sh`
# this can be much better, its a "make the job" version.
# for clean install Linux users,
# manual check all versions its crazy.!
# this simple .sh will show required version vs. installed
# will give .sh error if Not installed.
# But Is Not an error of the .sh

echo -e "========================================================="
echo -e "Program \tMinimal version "
echo -e "Program \tActual version "
echo -e "========================================================="
echo -e "user:group"
echo $(whoami):$(id -g -n)
echo -e "========================================================="
echo -e "gcc ($(uname -v)) 5.1"              
gcc --version | grep "gcc"
echo -e "========================================================="
echo -e "Clang/LLVM (optional) \t 11.0.0"     
clang --version
echo -e "========================================================="
echo -e "GNU Make 3.81"         
make --version | grep "Make"
echo -e "========================================================="
echo -e "GNU ld (GNU Binutils for Ubuntu) 2.23"            
ld -v | grep "Binutils"
echo -e "========================================================="
echo -e "flex 2.5.35"           
flex --version | grep "flex"
echo -e "========================================================="
echo -e "bison (GNU Bison) 2.0"              
bison --version | grep "bison" 
echo -e "========================================================="
echo -e "pahole v1.16"             
echo "pahole" $(pahole --version)
echo -e "========================================================="
echo -e "util-linux \t 2.10o"            
fdformat --version
echo -e "========================================================="
echo -e "kmod version 13"               
depmod -V | grep "kmod"
echo -e "========================================================="
echo -e "e2fsck 1.41.4 (dd-mmm-yyyy)"       
e2fsck -V
echo -e "========================================================="
echo -e "fsck.jfs version 1.1.3,"            
fsck.jfs -V | grep "fsck.jfs"
echo -e "========================================================="
echo -e "reiserfsck 3.6.3"
reiserfsck -V
echo -e "========================================================="
echo -e "xfs_db version 2.6.0"         
xfs_db -V | grep "xfs_db"
echo -e "========================================================="
echo -e "mksquashfs version 4.0 (yyyy/mm/dd)"              
mksquashfs -version | grep "mksquashfs"
echo -e "========================================================="
echo -e "btrfs-progs v0.18"             
# btrfsck 
btrfs --version | grep "btrfs-progs"
echo -e "========================================================="
echo -e "pcmciautils 004"              
pccardctl -V | grep "pcmciautils"
echo -e "========================================================="
echo -e "Quota utilities version 3.09."             
quota -V | grep "version"
echo -e "========================================================="
echo -e "pppd version 2.4.0"            
pppd --version
echo -e "========================================================="
echo -e "nfs-utils \t 1.0.5"            
showmount --version
echo -e "========================================================="
echo -e "ps from procps-ng 3.2.0"            
ps --version
echo -e "========================================================="
echo -e "udev \t 081"              
udevd --version
echo -e "========================================================="
# echo -e "grub\t 0.93"             
# grub --version
echo -e "grub-install (GRUB) 0.93"             
grub-install --version
echo -e "========================================================="
echo -e "mcelog \t 0.6"              
mcelog --version
echo -e "========================================================="
echo -e "iptables v1.4.2"            
iptables -V | grep "iptables"
echo -e "========================================================="
echo -e "OpenSSL 1.0.0 (Libcrypto: 1.0.0)"         
openssl version | grep "OpenSSL"
echo -e "========================================================="
echo -e "bc 1.06.95"      
bc --version | grep "bc"
echo -e "========================================================="
echo -e "sphinx-build 1.7"        
sphinx-build --version
echo -e "========================================================="
