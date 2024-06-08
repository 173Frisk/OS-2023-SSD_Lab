# OS-2023-SSD_Lab

Codes for completing the Fuse SSD lab work of OS, by Jen-Wei Hsieh, in NTUST, of year 2023.

## Features

* Implements [FAST (Fully Associative Sector Translation)](https://dl.acm.org/doi/10.1145/1275986.1275990)-like algorithm. In this case, we use log blocks like write buffers for all data blocks. Also, garbage collection is performed when all log blocks are full. This helps achieve low WAF (which is the goal of this lab work) while not relying on complicated code.
* Since RAM usage is more important than speed for this lab work, this program only uses one block size worth of cache memory while performing GC (garbage collection).

## Requirments

For Ubuntu:

```sh
apt-cache search fuse
sudo apt update
sudo apt install fuse3 libfuse3-dev
```

We recommand rebooting after the installation has finished.

## Usage

I'm writing this half a year after I've done this, so the below steps may be incomplete or incorrect. Sorry for any inconvenience...

1. Compile the code first:

    ```sh
    gcc -Wall ssd_fuse.c `pkg-config fuse3 --cflags --libs` -D_FILE_OFFSET_BITS=64 -o ssd_fuse
    gcc -Wall ssd_fuse_dut.c -o ssd_fuse_dut
    ```

2. Start SSD fuse lib with debug mode enabled

    2-1. Create dir by

    ```sh
    mkdir /tmp/ssd
    ```

    2-2. Mount at /tmp/ssd

    ```sh
    ./ssd_fuse –d /tmp/ssd
    ```

3. Start writing data!

## Contributions

If you find this repo helpful, we kindly ask you to stick a star sticker to our repo (star our repo)! No pressure though. 😉  
If you find any bugs, typos, or anything else that can help improve this program to meet the lab requirements, we welcome you to open issues or, even better, submit pull requests!
