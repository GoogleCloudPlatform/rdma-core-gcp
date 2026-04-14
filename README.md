# About this repo

## What is this repo?

This is a clone of the [upstream RDMA Core Userspace Libraries and Daemons](https://github.com/linux-rdma/rdma-core) with out-of-tree patches staged additionally in order for RDMA to work on Google Cloud Platform.

**This is not the final destination of GCP-specific RDMA core patches and GCP does not intend to indefinitely maintain out-of-tree changes.** GCP aims to upstream changes made in RDMA core, and this repo serves to stage those out-of-tree changes before they are fully ingested into the mainline. 

Eventually, we anticipate this repo to be essentially a mirror of the upstream if all major patches are accepted, with future developments happening in between.

## What does each branch do?

* The `master` branch is a pristine mirror of the upstream main branch for reference purposes, we expect to update it periodically.

* The `out-of-tree` branch continuously tracks the master branch, plus **OOT patches** applied, it can be a few versions behind upstream mainly for compatibility reasons.

* The `oot-v<XX>` branches are versioned releases, we will try to maintain stability and backport fixes/updates on a best-effort basis.

Other branches from the upstream (namely, stable branches) are omitted here for simplicity of reference and maintenance, they can be found in the [upstream RDMA core repo](https://github.com/linux-rdma/rdma-core).

## Pull requests and collaborations

This repo is mainly for staging and open-source purposes and doesn't expect community contributions, please directly contribute to the upstream repo instead.

---

> The original README begins here

[![Build Status](https://dev.azure.com/ucfconsort/rdma-core/_apis/build/status/linux-rdma.rdma-core?branchName=master)](https://dev.azure.com/ucfconsort/rdma-core/_build/latest?definitionId=2&branchName=master)

# RDMA Core Userspace Libraries and Daemons

This is the userspace components for the Linux Kernel's drivers/infiniband
subsystem. Specifically this contains the userspace libraries for the
following device nodes:

 - /dev/infiniband/uverbsX (libibverbs)
 - /dev/infiniband/rdma_cm (librdmacm)
 - /dev/infiniband/umadX (libibumad)

The userspace component of the libibverbs RDMA kernel drivers are included
under the providers/ directory. Support for the following Kernel RDMA drivers
is included:

 - bnxt_re.ko
 - efa.ko
 - erdma.ko
 - iw_cxgb4.ko
 - hfi1.ko
 - hns-roce-hw-v2.ko
 - ionic_rdma.ko
 - irdma.ko
 - ib_qib.ko
 - mana_ib.ko
 - mlx4_ib.ko
 - mlx5_ib.ko
 - ib_mthca.ko
 - ocrdma.ko
 - qedr.ko
 - rdma_rxe.ko
 - siw.ko
 - vmw_pvrdma.ko

Additional service daemons are provided for:
 - srp_daemon (ib_srp.ko)
 - iwpmd (for iwarp kernel providers)
 - ibacm (for InfiniBand communication management assistant)

# Building

This project uses a cmake based build system. Quick start:

```sh
$ bash build.sh
```

*build/bin* will contain the sample programs and *build/lib* will contain the
shared libraries. The build is configured to run all the programs 'in-place'
and cannot be installed.

### Debian Derived

```sh
$ apt-get install build-essential cmake gcc libudev-dev libnl-3-dev libnl-route-3-dev ninja-build pkg-config valgrind python3-dev cython3 python3-docutils pandoc
```

Supported releases:

* Debian 9 (stretch) or newer
* Ubuntu 16.04 LTS (xenial) or newer

### Fedora, CentOS 8

```sh
$ dnf builddep redhat/rdma-core.spec
```

NOTE: Fedora Core uses the name 'ninja-build' for the 'ninja' command.

### openSUSE

```sh
$ zypper install cmake gcc libnl3-devel libudev-devel ninja pkg-config valgrind-devel python3-devel python3-Cython python3-docutils pandoc
```

## Building on CentOS 7, Amazon Linux 2

Install required packages:

```sh
$ yum install cmake gcc libnl3-devel libudev-devel make pkgconfig valgrind-devel
```

Developers on CentOS 7 or Amazon Linux 2 are suggested to install more modern
tooling for the best experience.

CentOS 7:

```sh
$ yum install epel-release
$ yum install cmake3 ninja-build pandoc
```

Amazon Linux 2:

```sh
$ amazon-linux-extras install epel
$ yum install cmake3 ninja-build pandoc
```

NOTE: EPEL uses the name 'ninja-build' for the 'ninja' command, and 'cmake3'
for the 'cmake' command.

# Usage

To set up software RDMA on an existing interface with either of the available
drivers, use the following commands, substituting `<DRIVER>` with the name of
the driver of your choice (`rdma_rxe` or `siw`) and `<TYPE>` with the type
corresponding to the driver (`rxe` or `siw`).

```
# modprobe <DRIVER>
# rdma link add <NAME> type <TYPE> netdev <DEVICE>
```

Please note that you need version of `iproute2` recent enough is required for the
command above to work.

You can use either `ibv_devices` or `rdma link` to verify that the device was
successfully added.

# Reporting bugs

Bugs should be reported to the <linux-rdma@vger.kernel.org> mailing list
In your bug report, please include:

 * Information about your system:
   - Linux distribution and version
   - Linux kernel and version
   - InfiniBand hardware and firmware version
   - ... any other relevant information

 * How to reproduce the bug.

 * If the bug is a crash, the exact output printed out when the crash
   occurred, including any kernel messages produced.

# Submitting patches

See [Contributing to rdma-core](Documentation/contributing.md).

# Stable branches

Stable versions are released regularly with backported fixes (see Documentation/stable.md)
The current minimum version still maintained is 'v33.X'
