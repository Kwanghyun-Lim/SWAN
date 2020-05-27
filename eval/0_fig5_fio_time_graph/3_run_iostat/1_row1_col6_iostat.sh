

SSD0=/dev/sdb
SSD1=/dev/sdc
SSD2=/dev/sdd
SSD3=/dev/sde
SSD4=/dev/sdf
SSD5=/dev/sdg

sudo -s smartctl -A $SSD0 | tee 1r6c_sdb.smartctl
sudo -s smartctl -A $SSD1 | tee 1r6c_sdc.smartctl
sudo -s smartctl -A $SSD2 | tee 1r6c_sdd.smartctl
sudo -s smartctl -A $SSD3 | tee 1r6c_sde.smartctl
sudo -s smartctl -A $SSD4 | tee 1r6c_sdf.smartctl
sudo -s smartctl -A $SSD5 | tee 1r6c_sdg.smartctl

iostat -mp 1 | tee swan_1r6c_rW8K_6h_u40.iostat

