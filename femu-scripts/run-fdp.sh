#!/bin/bash -x
# Run FEMU as a black-box SSD (FTL managed by the device)

# image directory
IMGDIR=$HOME/images
# Virtual machine disk image
OSIMGF=$IMGDIR/u14s.qcow2

#NUM_CHANNELS=8
#NUM_CHIPS_PER_CHANNEL=4
#READ_LATENCY_NS=40000
#WRITE_LATENCY_NS=200000

# SSD capacity in MB
SSD_CAPACITY_MB=65536
# Number of RUH and Placement handle
NUM_PLACEMENT_HANDLE=8
NUM_RECLAIM_UNIT_HANDLE=8
NUM_PI_HANDLE=0
# Number of RG
NUM_RECLAIM_GROUP=1
# RU size in MB
RECLAIM_UNIT_SIZE_MB=256
# GC threshold
FDP_GC_THRES_PCENT=85
FDP_GC_THRES_PCENT_HIGH=95

FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${SSD_CAPACITY_MB}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
#FEMU_OPTIONS=${FEMU_OPTIONS}",zns_num_ch=${NUM_CHANNELS}"
#FEMU_OPTIONS=${FEMU_OPTIONS}",zns_num_lun=${NUM_CHIPS_PER_CHANNEL}"
#FEMU_OPTIONS=${FEMU_OPTIONS}",zns_read=${READ_LATENCY_NS}"
#FEMU_OPTIONS=${FEMU_OPTIONS}",zns_write=${WRITE_LATENCY_NS}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp_num_ph=${NUM_PLACEMENT_HANDLE}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp_num_pi=${NUM_PI_HANDLE}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp_num_ruh=${NUM_RECLAIM_UNIT_HANDLE}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp_num_rg=${NUM_RECLAIM_GROUP}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp_ru_size_mb=${RECLAIM_UNIT_SIZE_MB}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp_gc_thres_pcent=${FDP_GC_THRES_PCENT}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp_gc_thres_pcent_high=${FDP_GC_THRES_PCENT_HIGH}"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=4"
echo ${FEMU_OPTIONS}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

sudo ./qemu-system-x86_64 \
    -name "FEMU-FDPSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 32 \
    -m 32G \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::8080-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
