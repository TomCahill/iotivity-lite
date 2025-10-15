
# Building 
Clone down the iotivity-lite repo and checkout the `tom` branch
```
git clone --recursive git@github.com:TomCahill/iotivity-lite.git iotivity-lite-tom
cd iotivity-lite-tom
git checkout tom
```

Build the cloud server image
```
docker build -t cloud-server:latest -f docker/apps/Dockerfile.cloud-server-debug-single .
```

Build the onboarding tool for testing
```
mkdir build && cd build/
cmake -DCMAKE_BUILD_TYPE=Debug -DOC_TCP_ENABLED=1 -DOC_CLOUD_ENABLED=1 -DOC_OSCORE_ENABLED=0 -DOC_SECURITY_ENABLED=1 -DOC_PKI_ENABLED=1 ../
cmake --build . --target onboarding_tool
```

# Testing

Run the container and the onboarding tool
```
docker run -d --rm cloud-server:latest
./onboarding_tool/onboarding_tool
```
Input `1` then enter - Discover un-owned devices - **NOTE: This might take a while as the onboarding tool does a multicast discovery**  
Result: 
```
Discovered unowned device: 7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad at:
coap://172.17.0.3:46831
coaps://172.17.0.3:49994
coap+tcp://172.17.0.3:44551
coaps+tcp://172.17.0.3:35451
```

Input `7` then enter - Discover all resources on the device
Result:
```
My Devices:

Unowned Devices:
[0]: 7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad - CloudServer
```

Input `0` then enter - Should list the devices resources
Result:
```
anchor ocf://7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad, uri : /oic/p
anchor ocf://7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad, uri : /oc/con
anchor ocf://7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad, uri : /oic/res
anchor ocf://7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad, uri : /oic/d
anchor ocf://7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad, uri : /oc/wk/introspection
anchor ocf://7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad, uri : /oic/sec/doxm
anchor ocf://7ea4efd9-bc4c-4aae-54b5-5b4fd0b75dad, uri : /oic/sec/pstat
...
```

Now in the device logs we should see some output like the following:
```
[CS 2025-10-15T10:02:04.379685Z] <cloud_server_send_response_cb> method(1): GET, uri: /oic/sec/doxm, code(0): OC_STATUS_OK
[CS 2025-10-15T10:02:06.201657Z] <cloud_server_send_response_cb> method(1): GET, uri: /oic/res, code(0): OC_STATUS_OK
[CS 2025-10-15T10:02:06.202262Z] <cloud_server_send_response_cb> method(1): GET, uri: /oic/d, code(0): OC_STATUS_OK
```