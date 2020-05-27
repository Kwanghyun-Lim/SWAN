
SSD0=/dev/sdb
SSD1=/dev/sdc
SSD2=/dev/sdd
SSD3=/dev/sde
SSD4=/dev/sdf
SSD5=/dev/sdg

sudo -s smartctl -A $SSD0 | tee end-sdb.smartctl
sudo -s smartctl -A $SSD1 | tee end-sdc.smartctl
sudo -s smartctl -A $SSD2 | tee end-sdd.smartctl
sudo -s smartctl -A $SSD3 | tee end-sde.smartctl
sudo -s smartctl -A $SSD4 | tee end-sdf.smartctl
sudo -s smartctl -A $SSD5 | tee end-sdg.smartctl
