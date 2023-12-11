[ -d "./test/" ] && rm -rf ./test/
mkdir test


for i in {1..10000}; do echo $i >> /tmp/ssd1/ssd_file; done
./out/ssd_fuse_dut /tmp/ssd1/ssd_file r 80000 > ./test/ssd_dump_0.txt

for i in {1..100}; do ./out/ssd_fuse_dut /tmp/ssd1/ssd_file w 10 $(($i * 100)); done
./out/ssd_fuse_dut /tmp/ssd1/ssd_file r 80000 > ./test/ssd_dump_1.txt

for i in {1..10}; do ./out/ssd_fuse_dut /tmp/ssd1/ssd_file w 981 $(($i * 981)); done
./out/ssd_fuse_dut /tmp/ssd1/ssd_file r 80000 > ./test/ssd_dump_2.txt


for i in {1..10000}; do echo $i >> ./test/test.txt; done
./out/ssd_fuse_dut ./test/test.txt r 80000 > ./test/test_dump_0.txt
echo 1
for i in {1..100}; do ./out/ssd_fuse_dut ./test/test.txt w 10 $(($i * 100)); done
./out/ssd_fuse_dut ./test/test.txt r 80000 > ./test/test_dump_1.txt
echo 2
for i in {1..10}; do ./out/ssd_fuse_dut ./test/test.txt w 981 $(($i * 981)); done
./out/ssd_fuse_dut ./test/test.txt r 80000 > ./test/test_dump_2.txt

for i in {0..2}; do diff -s ./test/ssd_dump_$i.txt ./test/test_dump_$i.txt; done