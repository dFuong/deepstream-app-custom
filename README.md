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
- **deepstream-app: save_output + sink rtsp_out**
- **deepstream-rtsp-out: creating multi-rtsp-out**
- **runtime_source_add_delete: custome add_del input**
- Show output streaming (**RTSPServer**) with H265 streams

## Prerequisites

- DeepStreamSDK 5.1
- Python 3.6
- Gst-python

## Install

**Run with docker:**
- Docker pull for kernel architecture x86/amd64: `docker pull fsharp58/deepstream-app-custom:5.1_v1`
- Docker pull for kernel architecture x86/arm64 (jetson): `docker pull fsharp58/deepstream_custom:5.1.v1_l4t`
- Firstly running docker: `xhost +`
- Mount git-repo in docker follow: `-v <path to this directory> :/opt/nvidia/deepstream/deepstream/sources/python`


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

cd /folder {deepstream-app / deepstream-rtsp-out / runtime_source_add_delete}

make

deepstream-app
./deepstream-app -c config.txt

deepstream-rtsp-out
./deepstream-out file:/opt/nvidia/deepstream/deepstream-5.1/samples/streams/sample_720p.mp4 file:/opt/....
                 rtsp:/......     rtsp:/...........
                 
runtime_source_add_delete
./deepstream-out


```



