import sys
from PySide6.QtWidgets import QApplication, QMainWindow
from PySide6.QtCore import QTimer
import paho.mqtt.client as mqtt
from ui_form import Ui_MainWindow
import json
from datetime import datetime
import pytz

# 한국 시간대 (Asia/Seoul)
korea_timezone = pytz.timezone("Asia/Seoul")

# Broker(라즈베리파이5 또는 mosquitto를 띄운 PC) IP / Port
# 실제 브로커 IP로 수정 필요
address = "192.168.0.22"
port = 1883

# MQTT Topic
commandTopic = "RCCar/command"   # GUI -> (Broker) -> ESP32 : 제어 명령
sensingTopic = "RCCar/sensing"   # ESP32 -> (Broker) -> GUI : 센서값


class MainWindow(QMainWindow):
    # MQTT로 들어온 센서 data 전체
    sensorData = list()
    # 최신 15개만 표시할 list
    sensingDataList = list()
    # 보낼 command dict
    commandData = dict()
    # 보낸 command 이력
    commandDataList = list()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.ui = Ui_MainWindow()
        self.ui.setupUi(self)
        self.client = None
        self.timer = None
        self.init()

    def init(self):
        # 버튼 시그널 연결
        # (ui_form.py의 objectName에 맞춰 필요 시 이름 수정)
        self.ui.startButton.clicked.connect(self.start)
        self.ui.stopButton.clicked.connect(self.stop)
        self.ui.goButton.clicked.connect(self.go)
        self.ui.backButton.clicked.connect(self.back)
        self.ui.leftButton.clicked.connect(self.left)
        self.ui.rightButton.clicked.connect(self.right)
        self.ui.midButton.clicked.connect(self.mid)
        print('init')

    # ---------- MQTT 연결/시작 ----------
    def start(self):
        # 이미 연결돼 있으면 재연결 방지
        if self.client is not None:
            print('already started')
            return

        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message

        self.client.connect(address, port)
        self.client.subscribe(sensingTopic, qos=1)
        self.client.loop_start()

        # 0.5초마다 UI 갱신
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.settingUI)
        self.timer.start(500)

        print('start')
        self.publishCommand("start")

    # ---------- 공통 명령 발행 ----------
    def makeCommandData(self, cmd, arg, finish):
        current_time = datetime.now(korea_timezone)
        self.commandData["time"] = current_time.strftime("%Y-%m-%d %H:%M:%S")
        self.commandData["cmd_string"] = cmd
        self.commandData["arg_string"] = arg
        self.commandData["is_finish"] = finish
        return self.commandData

    def publishCommand(self, cmd, arg=100, finish=1):
        # 연결 안 된 상태에서 버튼 누르면 무시
        if self.client is None:
            print('not connected. press Start first.')
            return
        self.commandData = self.makeCommandData(cmd, arg, finish)
        self.client.publish(commandTopic, json.dumps(self.commandData), qos=1)
        self.commandDataList.append(self.commandData)
        self.commandData = dict()
        print(cmd, 'published')

    # ---------- 방향/정지 명령 ----------
    def go(self):
        self.publishCommand("go")

    def back(self):
        self.publishCommand("back")

    def left(self):
        self.publishCommand("left")

    def right(self):
        self.publishCommand("right")

    def mid(self):
        self.publishCommand("mid")

    def stop(self):
        self.publishCommand("stop")
        if self.timer is not None:
            self.timer.stop()
        if self.client is not None:
            self.client.loop_stop()
            self.client.disconnect()
            self.client = None
        print('stop')

    # ---------- UI 갱신 ----------
    def settingUI(self):
        self.ui.logText.clear()
        for i in range(len(self.commandDataList)):
            msg = "%3d | %s | %6s | %3d | %3d" % (
                i,
                self.commandDataList[i]["time"],
                self.commandDataList[i]["cmd_string"],
                self.commandDataList[i]["arg_string"],
                self.commandDataList[i]["is_finish"])
            self.ui.logText.appendPlainText(msg)

        self.ui.sensingText.clear()
        for i in range(len(self.sensingDataList)):
            sensingData = self.sensingDataList[i]
            msg = "%3d | %s | %3.2f | %3.2f | %3d" % (
                i + 1,
                sensingData["time"],
                sensingData["num1"],
                sensingData["num2"],
                sensingData["is_finish"])
            self.ui.sensingText.appendPlainText(msg)

    # ---------- MQTT 콜백 ----------
    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            print("connected OK")
        else:
            print("Bad connection Returned code=", reason_code)

    def on_message(self, client, userdata, msg):
        message = json.loads(msg.payload.decode("utf-8"))
        self.sensorData.append(message)
        self.sensingDataList = self.sensorData[-15:]

    # ---------- 종료 처리 ----------
    def closeEvent(self, event):
        if self.client is not None:
            self.publishCommand("exit")
            self.client.disconnect()

        current_time = datetime.now(korea_timezone)
        file_name = current_time.strftime("%Y-%m-%d") + ".txt"
        with open(file_name, "w") as file:
            for value in self.sensorData:
                file.write(str(value) + "\n")
        print("save data")
        event.accept()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    widget = MainWindow()
    widget.show()
    sys.exit(app.exec())
