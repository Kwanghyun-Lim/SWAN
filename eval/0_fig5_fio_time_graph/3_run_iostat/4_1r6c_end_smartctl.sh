
SSD0=/dev/sdb
SSD1=/dev/sdc
SSD2=/dev/sdd
SSD3=/dev/sde
SSD4=/dev/sdf
SSD5=/dev/sdg

sudo -s smartctl -A $SSD0 | tee 1r6c_end_sdb.smartctl
sudo -s smartctl -A $SSD1 | tee 1r6c_end_sdc.smartctl
sudo -s smartctl -A $SSD2 | tee 1r6c_end_sdd.smartctl
sudo -s smartctl -A $SSD3 | tee 1r6c_end_sde.smartctl
sudo -s smartctl -A $SSD4 | tee 1r6c_end_sdf.smartctl
sudo -s smartctl -A $SSD5 | tee 1r6c_end_sdg.smartctl
