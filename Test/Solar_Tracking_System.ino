#include <ESP32Servo.h>
#include <Fuzzy.h>

// Định nghĩa các chân cho 4 con LDR
const int LDR_TL = 32; // Trên - Trái (Top-Left)
const int LDR_TR = 33; // Trên - Phải (Top-Right)
const int LDR_BL = 34; // Dưới - Trái (Bottom-Left)
const int LDR_BR = 35; // Dưới - Phải (Bottom-Right)

// Thêm các biến toàn cục để lưu giá trị đã lọc cho 4 cảm biến
float filteredTL = 0, filteredTR = 0, filteredBL = 0, filteredBR = 0;

// Hệ số lọc alpha (0 < alpha < 1). 
// Alpha nhỏ (ví dụ 0.1): Lọc cực mượt nhưng đáp ứng chậm.
// Alpha lớn (ví dụ 0.5): Đáp ứng nhanh nhưng ít mượt hơn.
const float ALPHA = 0.2;

// Định nghĩa chân cho 2 Servo
const int SERVO_PAN_PIN = 12;  // Trục Ngang (Đế)
const int SERVO_TILT_PIN = 13; // Trục Dọc (Gật)

Servo servoPan;
Servo servoTilt;

// Các biến lưu góc hiện tại của Servo
float currentPanPos = 0;
float currentTiltPos = 90;

// Giới hạn góc quay để bảo vệ dây điện và cơ khí
const int PAN_MIN = 0;
const int PAN_MAX = 180;
const int TILT_MIN = 20;  // Không cho gập quá sâu
const int TILT_MAX = 160; // Không cho ngửa quá đà

// Ghìm tốc độ & Vùng chết an toàn 
const float MAX_STEP = 0.2;   // Ép motor nhích siêu chậm (tối đa 0.2 độ/lần) chống vọt lố
const float DEAD_BAND = 0.05; // Chốt chặn cơ học cuối cùng chống Hunting

// Bù trừ sai số nội trở của phần cứng (LDR Mismatch)
// Nếu hệ thống bị "lác" (không nhìn thẳng mặt trời), hãy tăng/giảm con số này
const int OFFSET_PAN = 0;  
const int OFFSET_TILT = 0; 

// Chúng ta cần 2 bộ xử lý mờ riêng biệt cho Trục Ngang và Trục Dọc
Fuzzy *fuzzyPan = new Fuzzy();
Fuzzy *fuzzyTilt = new Fuzzy();

// Hàm cài đặt Luật Mờ (Chạy trong setup)
void setupFuzzyLogic(Fuzzy *fuzzySystem, int offset) {
  // --- INPUT: Sai số ánh sáng (Error) ---
  // Sai số nằm trong khoảng -4095 đến 4095 (Do ESP32 đọc ADC 12-bit)
  FuzzyInput *errorInput = new FuzzyInput(1);

  // ĐÃ SỬA LỖI: Thêm tiền tố "err" để tránh xung đột với macro hệ thống (đặc biệt là biến PS)
  FuzzySet *errNB = new FuzzySet(-4095, -4095, -2000, -800 + offset); // Âm lớn (Lệch trái/dưới nhiều)
  FuzzySet *errNS = new FuzzySet(-2000, -800, -400 + offset, -100 + offset);    // Âm nhỏ (Lệch trái/dưới ít)
  FuzzySet *errZE = new FuzzySet(-150 + offset, -60 + offset, 60 + offset, 150 + offset);           // Cân bằng (Deadzone)
  FuzzySet *errPS = new FuzzySet(100 + offset, 400 + offset, 800, 2000);        // Dương nhỏ (Lệch phải/trên ít)
  FuzzySet *errPB = new FuzzySet(800, 2000, 4095, 4095);     // Dương lớn (Lệch phải/trên nhiều)

  errorInput->addFuzzySet(errNB);
  errorInput->addFuzzySet(errNS);
  errorInput->addFuzzySet(errZE);
  errorInput->addFuzzySet(errPS);
  errorInput->addFuzzySet(errPB);
  fuzzySystem->addFuzzyInput(errorInput);

  // --- OUTPUT: Bước nhảy góc (Delta Angle) ---
  // Bước nhảy từ -5 độ đến +5 độ mỗi chu kỳ
  FuzzyOutput *deltaAngle = new FuzzyOutput(1);

  FuzzySet *MoveFastNeg = new FuzzySet(-5, -5, -4, -2); 
  FuzzySet *MoveSlowNeg = new FuzzySet(-3, -1, -0.5, 0); 
  FuzzySet *Stop        = new FuzzySet(-0.8, -0.2, 0.2, 0.8); 
  FuzzySet *MoveSlowPos = new FuzzySet(0, 0.5, 1, 3);    
  FuzzySet *MoveFastPos = new FuzzySet(2, 4, 5, 5);    

  deltaAngle->addFuzzySet(MoveFastNeg);
  deltaAngle->addFuzzySet(MoveSlowNeg);
  deltaAngle->addFuzzySet(Stop);
  deltaAngle->addFuzzySet(MoveSlowPos);
  deltaAngle->addFuzzySet(MoveFastPos);
  fuzzySystem->addFuzzyOutput(deltaAngle);

  // --- HỆ LUẬT MỜ (RULE BASE) ---
  // Luật 1: NẾU Error là NB THÌ quay Lùi Nhanh
  FuzzyRuleAntecedent *ifNB = new FuzzyRuleAntecedent();
  ifNB->joinSingle(errNB);
  FuzzyRuleConsequent *thenMFN = new FuzzyRuleConsequent();
  thenMFN->addOutput(MoveFastNeg);
  fuzzySystem->addFuzzyRule(new FuzzyRule(1, ifNB, thenMFN));

  // Luật 2: NẾU Error là NS THÌ quay Lùi Chậm
  FuzzyRuleAntecedent *ifNS = new FuzzyRuleAntecedent();
  ifNS->joinSingle(errNS);
  FuzzyRuleConsequent *thenMSN = new FuzzyRuleConsequent();
  thenMSN->addOutput(MoveSlowNeg);
  fuzzySystem->addFuzzyRule(new FuzzyRule(2, ifNS, thenMSN));

  // Luật 3: NẾU Error là ZE THÌ Đứng yên
  FuzzyRuleAntecedent *ifZE = new FuzzyRuleAntecedent();
  ifZE->joinSingle(errZE);
  FuzzyRuleConsequent *thenStop = new FuzzyRuleConsequent();
  thenStop->addOutput(Stop);
  fuzzySystem->addFuzzyRule(new FuzzyRule(3, ifZE, thenStop));

  // Luật 4: NẾU Error là PS THÌ quay Tiến Chậm
  FuzzyRuleAntecedent *ifPS = new FuzzyRuleAntecedent();
  ifPS->joinSingle(errPS);
  FuzzyRuleConsequent *thenMSP = new FuzzyRuleConsequent();
  thenMSP->addOutput(MoveSlowPos);
  fuzzySystem->addFuzzyRule(new FuzzyRule(4, ifPS, thenMSP));

  // Luật 5: NẾU Error là PB THÌ quay Tiến Nhanh
  FuzzyRuleAntecedent *ifPB = new FuzzyRuleAntecedent();
  ifPB->joinSingle(errPB);
  FuzzyRuleConsequent *thenMFP = new FuzzyRuleConsequent();
  thenMFP->addOutput(MoveFastPos);
  fuzzySystem->addFuzzyRule(new FuzzyRule(5, ifPB, thenMFP));
}

// Hàm EMA Filter
float EMA_Filter(float current_raw, float last_filtered, float alpha) {
  return (alpha * current_raw) + ((1.0 - alpha) * last_filtered);
}

void setup() {
  Serial.begin(115200);

  // Cấu hình PWM cho Servo trên ESP32
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoPan.setPeriodHertz(50);
  servoTilt.setPeriodHertz(50);

  // Gắn chân Servo
  servoPan.attach(SERVO_PAN_PIN, 500, 2400);
  servoTilt.attach(SERVO_TILT_PIN, 500, 2400);

  // Đưa Servo về vị trí 90 độ (Home)
  servoPan.write(currentPanPos);
  servoTilt.write(currentTiltPos);
  delay(1000); // Chờ Servo về đúng vị trí

  // Khởi tạo Bộ điều khiển mờ cho 2 trục
  setupFuzzyLogic(fuzzyPan, OFFSET_PAN);
  setupFuzzyLogic(fuzzyTilt, OFFSET_TILT);


  filteredTL = analogRead(LDR_TL);
  filteredTR = analogRead(LDR_TR);
  filteredBL = analogRead(LDR_BL);
  filteredBR = analogRead(LDR_BR);
  
  Serial.println("Fuzzy Logic Solar Tracker Started!");
}

void loop() {
// 1. Đọc giá trị thô từ 4 LDR
  int rawTL = analogRead(LDR_TL);
  int rawTR = analogRead(LDR_TR);
  int rawBL = analogRead(LDR_BL);
  int rawBR = analogRead(LDR_BR);

  // 2. Lọc dữ liệu bằng EMA
  filteredTL = EMA_Filter(rawTL, filteredTL, ALPHA);
  filteredTR = EMA_Filter(rawTR, filteredTR, ALPHA);
  filteredBL = EMA_Filter(rawBL, filteredBL, ALPHA);
  filteredBR = EMA_Filter(rawBR, filteredBR, ALPHA);

// 3. Tính toán sai số sử dụng giá trị ĐÃ LỌC
  // Lưu ý: error vẫn có thể là int hoặc float tùy bạn, 
  // nhưng để độ chính xác mờ cao nhất, nên dùng float.
  float errorPan = (filteredTR + filteredBR) - (filteredTL + filteredBL); 
  float errorTilt = (filteredTL + filteredTR) - (filteredBL + filteredBR);

  // --- XỬ LÝ MỜ CHO TRỤC NGANG ---
  fuzzyPan->setInput(1, errorPan);
  fuzzyPan->fuzzify();
  float deltaPan = fuzzyPan->defuzzify(1); // Xuất ra góc nhích (-5 đến 5)

  // --- XỬ LÝ MỜ CHO TRỤC DỌC ---
  fuzzyTilt->setInput(1, errorTilt);
  fuzzyTilt->fuzzify();
  float deltaTilt = fuzzyTilt->defuzzify(1); // Xuất ra góc nhích (-5 đến 5)

  // 5. CHỐT CHẶN 1: Step Limiter (Ép motor bò từ từ, chống vọt lố)
  if (deltaPan > MAX_STEP) deltaPan = MAX_STEP;
  if (deltaPan < -MAX_STEP) deltaPan = -MAX_STEP;

  if (deltaTilt > MAX_STEP) deltaTilt = MAX_STEP;
  if (deltaTilt < -MAX_STEP) deltaTilt = -MAX_STEP;

  // 6. CHỐT CHẶN 2: Hardware Deadband (Chặn sai số li ti cuối cùng)
  if (abs(deltaPan) < DEAD_BAND) deltaPan = 0;
  if (abs(deltaTilt) < DEAD_BAND) deltaTilt = 0;

  // 7. Xuất lệnh đến Servo
  if (deltaPan != 0) {
    currentPanPos += deltaPan;
    currentPanPos = constrain(currentPanPos, PAN_MIN, PAN_MAX);
    servoPan.write(currentPanPos);
  }

  if (deltaTilt != 0) {
    currentTiltPos += deltaTilt;
    currentTiltPos = constrain(currentTiltPos, TILT_MIN, TILT_MAX);
    servoTilt.write(currentTiltPos);
  }

  // 6. In ra Serial Monitor để theo dõi và tinh chỉnh
  Serial.print("E_Pan: "); Serial.print(errorPan);
  Serial.print(" | dPan: "); Serial.print(deltaPan);
  Serial.print(" | E_Tilt: "); Serial.print(errorTilt);
  Serial.print(" | dTilt: "); Serial.print(deltaTilt);
  Serial.println();

  // Tốc độ lấy mẫu (100ms/lần giúp hệ thống mượt mà)
  delay(20); 
}
