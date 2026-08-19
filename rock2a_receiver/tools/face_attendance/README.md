# RV1106 / ROCK 2A 人脸考勤服务

该目录是独立于现有 AI 事件服务的考勤后端。它不启动摄像头、不保存请求 JPEG，且默认没有人脸验证器，因此不会把“检测到人脸”误当作已认证身份。

## 接口

`POST /v1/face/verify`，绑定到 ROCK 的有线地址 `192.168.50.1:9012`。

请求使用 `image/jpeg` body，并包含：

```text
X-Attendance-Token: <仅存放在两板本地配置中的共享令牌>
X-Attendance-Request-Id: face-00000001
X-Attendance-Type: check_in | check_out
X-Face-Width-Px: >=112
X-Face-Quality: 0.0..1.0
X-Camera-Id: rv1106-01
```

`request_id` 重发会返回同一条记录；同一人员、同一天、同一签到类型会返回 `duplicate`。SQLite 的唯一约束是最终去重保障。

## 开发期启动

```sh
mkdir -p runtime/face_attendance
printf '%s\n' 'replace-with-a-secret-at-least-16-chars' > runtime/face_attendance/token
python3 service.py \
  --database runtime/face_attendance/attendance.db \
  --token-file runtime/face_attendance/token
```

要启用 LBPH，ROCK 2A 必须安装 OpenCV Contrib，并指定训练好的模型：

```sh
python3 service.py --database runtime/face_attendance/attendance.db \
  --token-file runtime/face_attendance/token \
  --lbph-model runtime/face_attendance/lbph.yml
```

人员数据由受控的离线管理流程写入 `persons` 表；终端接口不提供录入能力。
开发期可在 ROCK 2A 本地初始化唯一测试人员（不通过网络暴露）：

```sh
python3 person_admin.py --database runtime/face_attendance/attendance.db \
  upsert 1 '测试用户' student
```
