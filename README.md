# Factory Monitoring System – Code Documentation

เอกสารนี้อธิบายโครงสร้างและการทำงานของ Firmware สำหรับระบบ Factory Monitoring บน ESP32-S3 โดยใช้เซ็นเซอร์ PMS5003 และ DHT22 ร่วมกับ Edge Impulse AI และ ThingsBoard Cloud เพื่อเฝ้าระวังสภาพแวดล้อมและแจ้งเตือนเมื่อพบความผิดปกติ

1. System Configuration (Config)

กำหนดค่าพารามิเตอร์หลักของระบบทั้งหมดไว้ใน Namespace Config เพื่อให้สามารถแก้ไขค่าต่าง ๆ ได้จากจุดเดียว

ประกอบด้วย:

Wi-Fi Configuration: SSID และ Password สำหรับเชื่อมต่อเครือข่าย
ThingsBoard Configuration: Server, MQTT Port, Access Token และ Client ID
Hardware Pin Configuration: กำหนด GPIO สำหรับ DHT22, PMS5003, LED และ Buzzer
Timing Configuration: กำหนดระยะเวลาการส่งข้อมูล การเชื่อมต่อใหม่ และการตรวจสอบเซ็นเซอร์
Threshold Configuration: กำหนดค่าเกณฑ์เริ่มต้นสำหรับ PM2.5, PM10, Temperature, Humidity และ Anomaly Score
2. Runtime Data Structures and State

กำหนดโครงสร้างข้อมูลและตัวแปรที่ใช้เก็บสถานะระหว่างการทำงานของระบบ

SensorReading

ใช้เก็บข้อมูลที่อ่านได้จากเซ็นเซอร์ ได้แก่

PM2.5
PM10
Temperature
Humidity
สถานะความถูกต้องของ PMS5003
สถานะความถูกต้องของ DHT22
สถานะความถูกต้องของข้อมูลทั้งหมด
AiResult

ใช้เก็บผลลัพธ์จากระบบ AI ได้แก่

anomaly_score คะแนนความผิดปกติ
confidence ความมั่นใจในการจำแนกประเภท
label ป้ายกำกับผลการจำแนก
status สถานะสุดท้ายของระบบ
RuntimeConfig

ใช้เก็บค่า Threshold ที่สามารถเปลี่ยนแปลงได้ระหว่างการทำงาน โดยรับค่าจาก ThingsBoard รวมถึงสถานะการ Mute Buzzer และสถานะการ Synchronize ค่า Threshold

3. LED and Buzzer Control System

ระบบควบคุมไฟ LED และ Buzzer เพื่อแสดงสถานะและแจ้งเตือนตามผลการประเมินสภาพแวดล้อม

LED Status

ใช้ฟังก์ชัน updateStatusLed() ในการกำหนดสีไฟตามสถานะของระบบ

Status	LED	GPIO
Normal	Green	GPIO 40
Warning	Blue	GPIO 41
Danger	Red	GPIO 42
Sensor Error	Blue	GPIO 41
Buzzer Control GPIO 18

ใช้ฟังก์ชัน tickBuzzer() ควบคุมการส่งเสียงแจ้งเตือน

Normal: ไม่มีเสียงแจ้งเตือน
Warning: ส่งเสียงสั้น 150 ms ทุก ๆ 2 วินาที
Danger: ส่งเสียง 200 ms และหยุด 200 ms สลับกัน
Muted: ปิดเสียง Buzzer ทุกสถานะ

นอกจากนี้ ระบบจะยังไม่เปิดใช้งาน Buzzer จนกว่าจะได้รับค่า Threshold จาก ThingsBoard เพื่อป้องกันการแจ้งเตือนโดยใช้ค่าเริ่มต้นก่อน Synchronization

4. Sensor Reading System

ทำหน้าที่อ่านข้อมูลจากเซ็นเซอร์ PMS5003 และ DHT22

PMS5003

ใช้ UART2 ในการรับข้อมูลจากเซ็นเซอร์ PMS5003 โดยมีขั้นตอนดังนี้

ตรวจสอบข้อมูลที่ได้รับจาก Serial2
ค้นหา Header ของข้อมูล 0x42 0x4D
อ่านข้อมูลจำนวน 32 Bytes
ตรวจสอบ Checksum เพื่อยืนยันความถูกต้องของข้อมูล
แยกค่าฝุ่น PM2.5 และ PM10 จาก Data Frame
บันทึกค่าล่าสุดไว้ใช้งาน

หากไม่มีข้อมูลใหม่เกิน 10 วินาที ระบบจะถือว่า PMS5003 ขาดการติดต่อ

DHT22

ใช้ Library DHT ในการอ่านค่าอุณหภูมิและความชื้น

ระบบจะตรวจสอบว่าค่าที่อ่านได้ไม่ใช่ NaN และอยู่ในช่วงข้อมูลที่กำหนด หากไม่ถูกต้องจะกำหนดสถานะ dhtValid เป็น False

Sensor Validation

ข้อมูลจากเซ็นเซอร์ทั้งสองจะถูกตรวจสอบร่วมกัน

pmsValid = สถานะ PMS5003 ปกติ
dhtValid = สถานะ DHT22 ปกติ
valid = เซ็นเซอร์ทั้งสองทำงานปกติ

หากเซ็นเซอร์ตัวใดตัวหนึ่งผิดปกติ ระบบจะไม่ส่งข้อมูลเข้าสู่กระบวนการ AI

5. Threshold Evaluation and Edge AI Processing

ส่วนประมวลผลหลักของระบบ ทำหน้าที่ประเมินสถานะจากข้อมูลเซ็นเซอร์และผลลัพธ์ของ AI

Threshold Evaluation

ฟังก์ชัน evaluateThresholdStatus() ใช้เปรียบเทียบข้อมูลเซ็นเซอร์กับค่า Threshold ที่กำหนดไว้

แบ่งออกเป็น 3 ระดับ

Danger: พบค่าที่เกินเกณฑ์อันตราย
Warning: พบค่าที่เกินเกณฑ์เฝ้าระวัง
Normal: ค่าทั้งหมดอยู่ในช่วงปกติ
Edge Impulse Inference

ฟังก์ชัน runEdgeImpulseInference() ทำหน้าที่ประมวลผลข้อมูลด้วยโมเดล Edge Impulse

ข้อมูลที่ส่งเข้าโมเดลประกอบด้วย 4 Features ตามลำดับ

Temperature
Humidity
PM2.5
PM10

จากนั้นระบบจะประมวลผลและดึงผลลัพธ์ ได้แก่

Classification Label
Classification Confidence
Anomaly Score
Anomaly Classification

ฟังก์ชัน classifyAnomalyStatus() ใช้ประเมินระดับความผิดปกติจาก Anomaly Score

Score ต่ำกว่า Warning Threshold = Normal
Score ตั้งแต่ Warning Threshold = Warning
Score ตั้งแต่ Danger Threshold = Danger
Final Status Evaluation

ฟังก์ชัน worstStatus() ใช้รวมผลจาก Threshold Evaluation และ AI Anomaly Detection โดยเลือกสถานะที่มีความรุนแรงสูงกว่าเป็นสถานะสุดท้าย

ตัวอย่างเช่น หาก Threshold ประเมินเป็น Normal แต่ AI ประเมินเป็น Danger ระบบจะกำหนดสถานะสุดท้ายเป็น Danger

6. Wi-Fi, MQTT and ThingsBoard Communication

จัดการการเชื่อมต่อเครือข่าย การรับส่งข้อมูล และการ Synchronize ค่า Configuration กับ ThingsBoard Cloud

Wi-Fi Connection

ฟังก์ชัน handleWiFiNonBlocking() ตรวจสอบสถานะ Wi-Fi และพยายามเชื่อมต่อใหม่ทุก 10 วินาทีเมื่อการเชื่อมต่อขาดหาย

MQTT Connection

ฟังก์ชัน handleMqttNonBlocking() จัดการเชื่อมต่อ MQTT กับ ThingsBoard Cloud

เมื่อเชื่อมต่อสำเร็จ ระบบจะ Subscribe Topics สำหรับรับ Attributes และ RPC พร้อมทั้ง Request Shared Attributes และส่งข้อมูล Client Attributes

Shared Attributes Synchronization

ฟังก์ชัน requestSharedAttributes() ใช้ร้องขอค่า Threshold ล่าสุดจาก ThingsBoard

ระบบจะร้องขอข้อมูลทุก 5 วินาที และนำค่าที่ได้รับมาอัปเดตใน RuntimeConfig

MQTT Message Handler

ฟังก์ชัน onMqttMessage() ทำหน้าที่รับและประมวลผลข้อความที่ส่งมาจาก ThingsBoard

การทำงานประกอบด้วย

รับค่า Threshold ใหม่
อัปเดตค่าการแจ้งเตือน
รับคำสั่ง Mute และ Unmute Buzzer
ประมวลผลสถานะใหม่ทันทีเมื่อค่า Threshold เปลี่ยนแปลง
ตอบกลับ RPC Request ด้วยสถานะการทำงาน
Telemetry Publishing

ฟังก์ชัน publishTelemetry() ทำหน้าที่ส่งข้อมูลเซ็นเซอร์และผลการประมวลผล AI ไปยัง ThingsBoard ผ่าน MQTT

ข้อมูลที่ส่งประกอบด้วย

PM2.5 และ PM10
Temperature และ Humidity
AI Status และ AI Label
Anomaly Score และ AI Confidence
Threshold Synchronization Status
Buzzer Mute Status
Wi-Fi RSSI
System Uptime

ระบบส่งข้อมูลทุก 3 วินาที โดยจะเลือกส่งข้อมูลตามสถานะความพร้อมของเซ็นเซอร์ เพื่อให้สามารถตรวจสอบความผิดปกติของเซ็นเซอร์จาก Dashboard ได้

7. Watchdog, Setup and Main Loop

ส่วนเริ่มต้นระบบและควบคุมการทำงานหลักของ Firmware

Watchdog Timer

ฟังก์ชัน initWatchdog() ใช้ตั้งค่า Watchdog Timer โดยกำหนด Timeout ไว้ที่ 15 วินาที

ภายใน Main Loop จะเรียก esp_task_wdt_reset() เพื่อแจ้งว่าระบบยังทำงานตามปกติ

หากโปรแกรมค้างจนไม่สามารถ Reset Watchdog ได้ ระบบจะเข้าสู่กระบวนการ Watchdog Timeout

Setup()

ทำหน้าที่เริ่มต้นอุปกรณ์และตั้งค่าระบบก่อนเข้าสู่ Main Loop

ลำดับการทำงานประกอบด้วย

เริ่มต้น Serial Monitor
ตั้งค่า UART2 สำหรับ PMS5003
เริ่มต้น DHT22
กำหนด GPIO สำหรับ LED และ Buzzer
เริ่มต้น Watchdog Timer
ตั้งค่า Wi-Fi Auto Reconnect
กำหนด MQTT Buffer Size
กำหนด ThingsBoard MQTT Server
ตั้งค่า MQTT Callback สำหรับรับข้อความ
Main Loop

ฟังก์ชัน loop() เป็นส่วนควบคุมการทำงานหลักของระบบ โดยทำงานวนซ้ำอย่างต่อเนื่อง

ลำดับการทำงานประกอบด้วย

Reset Watchdog Timer
ตรวจสอบและจัดการการเชื่อมต่อ Wi-Fi
ตรวจสอบและจัดการการเชื่อมต่อ MQTT
ประมวลผล MQTT Message
ตรวจสอบและควบคุมการทำงานของ Buzzer
Request Shared Attributes ทุก 5 วินาที
อ่านข้อมูลจาก PMS5003 และ DHT22 ทุก 3 วินาที
ส่งข้อมูลเข้าสู่กระบวนการ Edge AI
ประเมินสถานะ Normal, Warning หรือ Danger
อัปเดตสถานะ LED และ Buzzer
ส่งข้อมูล Telemetry ไปยัง ThingsBoard
Overall System Workflow

ภาพรวมการทำงานของระบบตั้งแต่การอ่านข้อมูลเซ็นเซอร์จนถึงการส่งข้อมูลขึ้น Cloud

text
       ESP32-S3 Startup
              |
              v
      Initialize Hardware
              |
              v
       Connect to Wi-Fi
              |
              v
     Connect ThingsBoard MQTT
              |
              v
    Synchronize Thresholds
              |
              v
     Read Sensor Data
       /          \
      v            v
   PMS5003        DHT22
      \            /
       v          v
     Validate Sensor Data
              |
              v
       Sensor Data Valid?
         /          \
       No            Yes
       |              |
       v              v
  Sensor Error    Edge Impulse AI
                      |
                      v
              Threshold Evaluation
                      |
                      v
               Combine AI Result
                      |
                      v
              Determine Final Status
                      |
           +----------+----------+
           |          |          |
           v          v          v
         Normal     Warning     Danger
           |          |          |
           v          v          v
        Green LED   Blue LED   Red LED
           |          |          |
           +----------+----------+
                      |
                      v
                Buzzer Control
                      |
                      v
              Publish Telemetry
                      |
                      v
               ThingsBoard Cloud
                      |
                      v
                 Repeat Loop

Summary

Firmware นี้แบ่งการทำงานออกเป็น 8 ส่วนหลัก ได้แก่ Configuration, Runtime State, LED & Buzzer Control, Sensor Reading, Threshold & AI Processing, Wi-Fi & MQTT Communication และ Watchdog & Main Loop

ทุกส่วนทำงานร่วมกันเพื่อให้ ESP32-S3 สามารถตรวจวัดสภาพแวดล้อม วิเคราะห์ความผิดปกติด้วย Edge AI แจ้งเตือนผ่านอุปกรณ์ภายใน และส่งข้อมูลไปยัง ThingsBoard Cloud ได้อย่างต่อเนื่อง
