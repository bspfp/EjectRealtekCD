# EjectRealtekCD
Realtek 8821CU WIFI/Bluetooth 유틸

## 용도
USB 타입의 Realtek 8821CU 칩을 사용하는 Wifi+Bluetooth 제품을 사용할 때, 디바이스 드라이버를 설치했음에도 불구하고 PC를 껐다가 켤 떄 USB CDROM으로 인식되는 문제가 발생합니다.

이 때, 이 프로그램을 컴퓨터 켜 질 때 실행되도록 작업 스케쥴러에 등록해 두면 CDROM을 제거하고 Wifi+Bluetooth를 정상적으로 사용할 수 있습니다.

## 참고 사항
Windows의 Fast Boot (빠른 부팅) 기능을 사용하는 경우
작업 스케쥴러의 시작 시 실행하는 트리거가 실행되지 않을 수 있습니다.
이 때에는 적절한 다른 Windows 이벤트에 트리거를 걸어서 사용해야 합니다.

예를 들면 Kernel-Power 이벤트 ID 107번인 **시스템이 절전 모드에서 다시 시작되었습니다.** 이런 것으로...

## 코드 소개

### USB 인식 상태 확인
USB가 CDROM으로 인식되지 않고 정상적으로 Wifi로 인식되어 네트워크에 잘 연결되었는지를 확인하여 동작을 결정할 떄 getaddrinfo()를 사용해 특정 주소(bspfp.pe.kr)의 IP를 확인하도록 하였습니다.
```cpp
optional<bool> TestNetwork() {
	static unordered_set<int> RetryErrorSet = { 11001, };
	addrinfo hints = {};
	addrinfo* results = nullptr;
	hints.ai_flags = AI_BYPASS_DNS_CACHE;
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if (getaddrinfo(TestTargetHost, nullptr, &hints, &results) != 0) {
		auto lasterr = WSAGetLastError();
		if (RetryErrorSet.find(lasterr) != RetryErrorSet.end())
			return false; // 다시 시도해야 함
		return {}; // 오류로 진행 불가
	}
	return true; // 네트워크 정상
}
```

### 볼륨 확인
- 엉뚱한 디바이스가 제거되지 않도록 GetVolumeInformationW()을 사용해 볼륨 이름을 확인합니다.
- CreateFileW()로 파일명을 "**\\.\F:**"과 같이 주어 파일을 열고 DeviceIoControl()를 통해서
  1. FSCTL_LOCK_VOLUME 볼륨을 잠그고
  1. FSCTL_DISMOUNT_VOLUME 마운트를 풀고
  1. IOCTL_STORAGE_MEDIA_REMOVAL 제거하고
  1. IOCTL_STORAGE_EJECT_MEDIA 꺼냅니다