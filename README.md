 # Table of Contents

 - Introduction
 - Prerequisites
 - Install
 - How to use
 - Result

## Introduction

- This repo is first custom deepstream:v5.1 (human detection) for **multiple source**
- Model can be trained by [TLT:v2.0_py3](https://ngc.nvidia.com/catalog/containers/nvidia:tlt-streamanalytics) or pretrained-model [PeopleNet](https://ngc.nvidia.com/catalog/models/nvidia:tlt_peoplenet)
- Another model can be trained by [TAO-upgrade TLT](https://ngc.nvidia.com/catalog/containers/nvidia:tao:tao-toolkit-tf)
- Show output streaming (**RTSPServer**) with H265 streams

## Prerequisites

- DeepStreamSDK 5.1
- Python 3.6
- Gst-python

## Install

**Run with docker:**
- Docker pull for kernel architecture x86/amd64: `docker pull fsharp58/deepstream_custom:5.1_v2`
- Docker pull for kernel architecture x86/arm64 (jetson): `docker pull fsharp58/deepstream_custom:5.1.v1_l4t`
- Firstly running docker: `xhost +`
- Mount git-repo in docker follow: `-v <path to this directory> :/opt/nvidia/deepstream/deepstream/sources/python`

**Run with your host:**
- Follow README in branch: 2.0.1

**Dependencies**
------------
 `$sudo apt-get update`
 
Kafka:
 ------
    $ sudo apt-get install python3-pip
    $ pip3 install kafka-python

- Set up Kafka following docker:[Kafka-docker](https://forums.developer.nvidia.com/t/using-kafka-protocol-for-retrieving-data-from-a-deepstream-pipeline/67626/14)
- Set up Kafka following Kafka-enviroment:[Kafka-enviroment](https://kafka.apache.org/quickstart)


## How to use

**SETUP**

1.Running kafka-enviroment

- For docker: `$ docker-compose up`
- For enviroment: To run following step **Install-Dependencies** 

2.To run message-kafka _consumer_

- Receive single message-kafka: `$ python3 consumer.py`
- Receive multiple message-kafka: Need to creating multi file **consumer_{}.py** as same as **consumer.py**
```
$ python3 consumer.py
$ python3 consumer_1.py

$ python3 .............
```
3.To run multi-rtsp-output

- You receive link rtspServer defined at **deepstream_rtsp_demux.py**, eg: **rtsp://localhost:rtspPort/rtspPath**
- Running file: `$ python3 rtsp-out.py rtsp://rtspIP:8554/rtspPath rtsp://rtspIP:8554/rtspPath`


**To run:**
```
**For without RTSPServer:**

python3 deepstream_mul.py file:<path to video(H-265)> file:<path to video(H-265)> 
python3 deepstream_mul.py rtsp:// rtsp:// 

**For with RTSPServer:**

python3 deepstream_rtsp_nondemux.py rtsp:// rtsp:// 
or
python3 deepstream_rtsp_demux rtsp:// rtsp://

**For add_del input stream + RTSP-out:***
python3 deepstream_add_del_mul_out.py 

```
## Result 
- Link drive: [Video-inference](https://drive.google.com/drive/folders/1L25-oGKAWSzZYbbt49WzFzjlHnMs0SSU?usp=sharing)
- Link benchmark performance: [FPS](https://gitlab.com/futureai/intrusion-detection/-/tree/benchmark_jetson) 


