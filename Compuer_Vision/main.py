import cv2
from urllib.request import urlopen
import numpy as np
import random
from ultralytics import YOLO
import time
import requests


class StreamCV:
    def __init__(self, send_req_url: str, stream_url: str) -> None:
        """Initialization function for important parameters"""
        
        self.send_req_url = send_req_url
        self.stream_url = stream_url
        self.person_follow_mode = False
        self.curr_follow_mode = "servo"
        
        try:
            self.class_list = open("utils/coco.txt", "r").read().split("\n")
        except FileNotFoundError:
            self.class_list = [] 
        self.detection_colors = [(random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
                                  for _ in range(len(self.class_list))]
        
        self.model = YOLO("weights/yolov8n.pt", "v8")
        self.running = True
        self.prev_frame_time = 0
        self.person_in_frame = False
        
    def _get_frame(self):
        """Returns current frame from stream url"""
        
        try:
            img_resp = urlopen(self.stream_url, timeout=2) # Added timeout for safety
            imgnp = np.asarray(bytearray(img_resp.read()), dtype="uint8") 
            return cv2.imdecode(imgnp, -1)
        except Exception as e:
            print(f"Error getting frame: {e}")
            return None
    
    def _draw_fps(self, frame) -> None:
        """Draws fps on frame"""
        
        new_frame_time = time.time()
        diff = new_frame_time - self.prev_frame_time
        fps = 1/diff if diff > 0 else -1
        self.prev_frame_time = new_frame_time
        fps_show = "FPS: " + str(int(fps))
        cv2.putText(frame, fps_show, (10, 50), cv2.FONT_HERSHEY_COMPLEX, 1, (255, 255, 255), 2, cv2.LINE_AA)
        
    def _draw_bb(self, frame) -> tuple[int,int]:
        """Draws detected objects on frame"""
        
        self.person_in_frame = False
        x_point, y_point = None, None
        
        detect_params = self.model.predict(source=[frame], conf=0.5, save=False)
        if len(detect_params[0]) == 0:
            return x_point, y_point
        
        for box in detect_params[0].boxes:
                clsID = int(box.cls[0])
                conf = float(box.conf[0])
                bb = box.xyxy[0].cpu().numpy().astype(int)
        
                should_draw_box = (not self.person_follow_mode) or (self.person_follow_mode and clsID == 0)

                if should_draw_box:
                    color = self.detection_colors[clsID] if clsID < len(self.detection_colors) else (255,255,255)
                    cv2.rectangle(frame, (bb[0], bb[1]), (bb[2], bb[3]), color, 3)
                    
                    label = self.class_list[clsID] if clsID < len(self.class_list) else "Unknown"
                    cv2.putText(frame, f"{label} {conf:.2f}%", (bb[0], bb[1] - 10), 
                                cv2.FONT_HERSHEY_COMPLEX, 0.75, (255, 255, 255), 1)

                if clsID == 0 and self.person_follow_mode:
                    x_point = int(((bb[2]-bb[0])/2)+bb[0])
                    y_point = int(((bb[3]-bb[1])/2)+bb[1])

                    cv2.line(frame, (x_point, 0), (x_point, frame.shape[0]), (0,255,0), 2)
                    cv2.line(frame, (0, y_point), (frame.shape[1], y_point), (0,255,0), 2)
                    self.person_in_frame = True
                
        return x_point, y_point
    
    def _draw_text(self, frame) -> None:
        """Draws text on screen depending on current mode"""
        
        if self.person_follow_mode:
            mode_str = "Car follow" if self.curr_follow_mode == "motor" else "Servo"
            text = f"Follow person mode ({mode_str})"
        else:
            text = "Object detection mode"  
        cv2.putText(frame, text, (10, 80), cv2.FONT_HERSHEY_COMPLEX, 0.8, (255, 255, 255), 2, cv2.LINE_AA)
    
    def _follow_person(self, x_point: int, y_point: int) -> None:
        """Sends movement commands to the robot"""
        
        instr = None
        if self.curr_follow_mode == "servo": 
            if x_point < 220: instr = "servol"
            elif x_point > 420: instr = "servor"
            
        else:
            if x_point <= 150: instr = "left"
            elif x_point >= 490: instr = "right"
            elif 150 < x_point < 490: instr = "fw"

        if instr:
            try:
                requests.post(self.send_req_url, data=instr, timeout=0.1)
            except requests.exceptions.RequestException:
                pass

    def handle_input(self) -> None:
        """Handles keyboard input"""
        
        key = cv2.waitKey(1)
        if key == 27:
            self.running = False
        elif key == 32:
            self.person_follow_mode = not self.person_follow_mode
        elif key == 9 and self.person_follow_mode:
            if self.curr_follow_mode == "motor":
                self.curr_follow_mode = "servo"
            else:
                self.curr_follow_mode = "motor"
                try:
                    requests.post(self.send_req_url, data="rstservo", timeout=0.1)
                except: pass

    def run(self) -> None:
        """Main loop logic"""
        
        while self.running:
            frame = self._get_frame()
            if frame is None: continue
            self._draw_fps(frame)
            x_point, y_point = self._draw_bb(frame)
            self._draw_text(frame)
            if self.person_follow_mode and self.person_in_frame and x_point is not None:
                self._follow_person(x_point, y_point)
            if self.person_follow_mode and not self.person_in_frame:
                try:
                    requests.post(self.send_req_url, data="stop", timeout=0.1)
                except: pass
            cv2.imshow("ObjectDetection", frame)
            self.handle_input()
            
        cv2.destroyAllWindows()
        
# stream = StreamCV("---", "---")
# stream.run()