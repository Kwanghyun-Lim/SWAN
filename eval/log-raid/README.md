# DM-SRC
DM-SRC (SSD-based RAID as Cache) is a block level cache layer for Linux DM (Device Mapper) framework.
Specifically, DM-SRC provides high-performance and write-back reliability by using multiple SSD devices and RAID protection scheme in contrast to FlashCache and BCache that support use of a single SSD as a cache device.
This project is based on DM-WRITEBOOST. Thanks to Akira.

* This project will be officially released soon. (Not Released)

## Supported Features
* Multiple SSD devices 
* Log-structured layout that converts random writes to sequential ones efficientely
* Erasure coding schemes (i.e, RAID-4, -5, -6)
* Separated striping scheme (cached dirty data are only protected by erasure coding schemes, while clean data are not. The reason is that clean data are copied from the primary storage.)
* Sequential I/O detection mechanism

![Alt text](http://embedded.uos.ac.kr/~ysoh/dmsrc/dmsrc_architecture.png)

## Unupported Features
* Online recovery procedure for cache device failures
* Online cache device replacement
* Quick metadata scanning at a mouting time

## References
* Yongseok Oh et al., SSD RAID as Cache (SRC) with Log-structured Approach for Performance and Reliability, Presented at IBM Watson Research Center (2014) 
  [slide](http://embedded.uos.ac.kr/~ysoh/DM-SRC-IBM.pdf)
* Yongseok Oh et al., Improving Performance and Lifetime of the SSD RAID-based Host Cache through a Log-Structured Approach (2013) 
  [slide](https://c2a2b76b-a-62cb3a1a-s-sites.googlegroups.com/site/2013inflow/home/presentations/INFLOW13_oh_SRC_presentation_web.pptx.pdf?attachauth=ANoY7cqgASxDavHemjtEYn73rvm3WRx_zFYTi-nlnrdMQ6BQuti86_88-fNrJ7pC6YgA45ueqU67aZOmoVIrSrdCCbwoZh-fv6E41bAzXm1lVogsXAUejx_FYL0fkw9mbX4iZdRWfF9TpCPeDG3VNhmwFH1q08qfyU8GuXx-Dd4f8X6hdUWs27u2CfJn_I5HBdnDdGikWNA8KNe5wq7y05F_o-IemsBEwza_UrMzk4gAe7xgQpGawLCV1lA8o_9kOznzJWMp9PHzqewFJuHvz1EH6yjaYKN21A%3D%3D&attredirects=0)
  [paper](https://c2a2b76b-a-62cb3a1a-s-sites.googlegroups.com/site/2013inflow/home/accepted_papers/oh_improvingPerformanceAndLifetime_inflow2013.pdf?attachauth=ANoY7cpxgVEtO-ERgXJjBDyW429GDWlV8C_0oXEUXYaUOty2L4j9NPEdGYjKMHZmtbdOeH7VhyTlj-vk_ZZI92E1Evu7aeUq0OqPBdYPfV7i_-8RMjFShkKjjWkZYgW05OC3oCAV4VXLNw5KfSUB07GRzfjxlM4jjwR41zEHGU0QH2wL5X71VLPdeOL1XXTXEtumgEBQeueD2Lz6Y19Cvzmjyb_lmBSCCmtgq_0QkIPiQTpzlGcQYwXJXGSIn98nw9619QFGE_vhFlCGu6C66LMfs8KZfxBxbtWwFtk1g3Vgsgp9wFekoIg%3D&attredirects=0)

## Quick Start
You are ready for nice scripts for quick starting.  

(1) Configure the path for the devices

	$ cd dm-src 
	$ vi config

(2) Compile

	$ source build

(3) Load

	$ cd Driver
	$ insmod dm-src.ko

(3) Run mount script (Edit if you want)

	$ cd util
	$ source mount_dmsrc [backing device] [cache device 0] ... [cache device N]
	  (Ex. $ source mount_dmsrc /dev/sdb1 /dev/sdc1 /dev/sdd1 /dev/sde1 /dev/sdf1)

(4) Test

	$ dd if=/dev/zero of=/dev/mapper/dmsrc-vol  

(5) Umount

	$ cd util
	$ source umount_dmsrc


## Developer Info
Yongseok Oh (ysoh@uos.ac.kr)
