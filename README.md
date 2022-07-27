# gr-ccsds

this is a GNU Radio module for processing data which is encoded according to the [CCSDS][ccsds] 131.0-B standard.
it handles Reed Solomon, interleaving and scrambling/randomization.

it originally was done as part of my master thesis at [NTNU][ntnu] in the spring of 2016.

## installing

to use the blocks, you need to install GNU Radio.

the simplest method on Ubuntu 16.04 is to use 

    sudo apt install git cmake swig gnuradio
    
or with [PyBOMBS][pybombs] if you want the newest version

then clone this repo and follow these instructions

    mkdir build/
    cd build/
    cmake .. -DCMAKE_INSTALL_PREFIX=$(gnuradio-config-info --prefix)
    make
    [sudo] make install

[ccsds]: https://public.ccsds.org/Publications/BlueBooks.aspx
[ntnu]: https://ntnu.edu
[pybombs]: https://github.com/gnuradio/pybombs
    
## GNURadio Conference Talk

I presented my additions to this OOT module. My contribution was adding convolutional coding to create a nearly CCSDS-compliant full-duplex modem. I cheat a little by using my own toy encapsulation and link layer protocols.

See the presentation here: https://drive.google.com/open?id=0B-CV_07uSBIuVXBBdzRZYWw1cG8

And the video here: https://www.youtube.com/watch?v=gVCCQ6rX3nk

## IP Encapsulation Instructions

You need to run a flowgraph on two separate systems, each with their own USRP, with TX/RX and RX2
connectors cross-connected, with approriate attenuation (at least 40 dB).  The flowgraphs
set up a TUN device, so you have to run them as root (use sudo).

On system 1:

`./concatenated_qpsk_modem_txrx.py --mtu 1115 --rx-freq=400M --tx-freq=415M --gain 45 --tx-user-data-rate=500000 --rx-user-data-rate=500000`

On system 2:

`./concatenated_qpsk_modem_txrx.py --mtu 1115 --rx-freq=415 --rx-freq=400M --gain 45 --rx-user-data-rate=500000 --tx-user-data-rate=500000`

Now you need to setup the TUN devices' properties.  This sets up a point-to-point network between
the systems.  You can adjust the IP addresses to avoid conflicts with any network you are using.

On system 1:

`sudo ifconfig tun1 10.11.10.1 pointopoint 10.11.10.2`

On system 2:

`sudo ifconfig tun1 10.11.10.2 pointopoint 10.11.10.1`

Now you can network between the systems, over the RF link.  For example:

From system 1:

`ping -A 10.11.10.2`

`ssh -vvv user@10.11.10.2`

`scp -rv file user@10.11.10.2`

Note that file transfer may stall.  This may be due to the TCP/IP stack congestion window being 
reduced when it mistakenly identifies a packet loss or long RTT time as congestion on a normal 
network.  We should be able to fix this with a PEP.  Another possibility is that we're sending 
a packet that exceeds the MTU of the TUN, but I don't think this should be possible.

I usually use the following custom settings for OTA SSH (just add them to your ssh config file 
for the link you want to use:

```
Host usrp
Hostname 10.11.10.2
User hawk
Port 22
StrictHostKeyChecking no
#PreferredAuthentications password
#PubkeyAuthentication no
#PasswordAuthentication yes
ChallengeResponseAuthentication no
CheckHostIP no
ConnectTimeout 300
ConnectionAttempts 10
ServerAliveInterval 15
ServerAliveCountMax 4
TCPKeepAlive no
Cipher blowfish-cbc
```

More fun:

`rsync -rzhPit --bwlimit=45k --stats --sockopts tcp_frto=1,tcp_frto_response=3,tcp_slow_start_after_idle=0,tcp_keepalive_probes=100,tcp_keepalive_intvl=3,tcp_low_latency=1,TCP_CONGESTION=vegas,TCP_MAXSEG=1113 Downloads/AIS.SampleData.zip  usrp:~/Downloads/`

It's often a good idea to keep some backround pinging going.  Try not to overwhelm the link though, unless you're using some sort of QOS

`ping -i 1 10.11.10.2`

# Network Test (no hardware)

`./concatenated_qpsk_modem_txrx_net.py --mtu 1115 --source-ip 192.168.1.231 --dest-ip 192.168.1.223 --source-port 52001 --dest-port 52002 --rx-user-data-rate 1000000 --tx-user-data-rate 1000000`

`./concatenated_qpsk_modem_txrx_net.py --mtu 1115 --source-ip 192.168.1.223 --dest-ip 192.168.1.231 --dest-port 52001 --source-port 52002 --rx-user-data-rate 1000000 --tx-user-data-rate 1000000`
