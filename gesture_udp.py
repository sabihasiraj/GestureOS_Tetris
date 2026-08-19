import cv2
import time
import math
import socket
from collections import deque
import mediapipe as mp

# ESP32 UDP settings
ESP32_IP = "192.168.0.112"
ESP32_PORT = 4210

sock = socket.socket(
    socket.AF_INET,
    socket.SOCK_DGRAM
)

# Send one command to ESP32
def send_command(command):
    try:
        sock.sendto(
            command.encode(),
            (
                ESP32_IP,
                ESP32_PORT
            )
        )

        print("Sent:", command)
        return True

    except OSError as error:
        print(
            "ESP32 unreachable:",
            command,
            "-",
            error
        )

        return False

# Static gestures are one-shot
last_static_pose = ""

# Neutral timing prevents accidental duplicate gestures
neutral_started = None
NEUTRAL_REARM_TIME = 0.15

# Blocks static gestures immediately after DROP
drop_static_lock = False

# Send LEFT, RIGHT, ROTATE, or PAUSE only once per pose
def register_static_pose(pose):
    global last_static_pose
    global neutral_started

    neutral_started = None

    if pose == last_static_pose:
        return

    if send_command(pose):
        last_static_pose = pose

# Re-arm the previous gesture after a short neutral position
def register_neutral(current_time):
    global last_static_pose
    global neutral_started
    global drop_static_lock

    if neutral_started is None:
        neutral_started = current_time
        return

    if (
        current_time - neutral_started
        >= NEUTRAL_REARM_TIME
    ):
        last_static_pose = ""
        drop_static_lock = False

# Removing the hand immediately re-arms gestures
def register_no_hand():
    global last_static_pose
    global neutral_started
    global drop_static_lock

    last_static_pose = ""
    neutral_started = None
    drop_static_lock = False

# MediaPipe setup
BaseOptions = mp.tasks.BaseOptions
HandLandmarker = mp.tasks.vision.HandLandmarker
HandLandmarkerOptions = mp.tasks.vision.HandLandmarkerOptions
RunningMode = mp.tasks.vision.RunningMode

options = HandLandmarkerOptions(
    base_options=BaseOptions(
        model_asset_path="hand_landmarker.task"
    ),
    running_mode=RunningMode.VIDEO,
    num_hands=1,
    min_hand_detection_confidence=0.5,
    min_hand_presence_confidence=0.5,
    min_tracking_confidence=0.5
)

# Calculate distance between two landmarks
def distance(a, b):
    return math.sqrt(
        (a.x - b.x) ** 2 +
        (a.y - b.y) ** 2
    )

# Calculate angle ABC
def angle(a, b, c):
    ab = (
        a.x - b.x,
        a.y - b.y
    )

    cb = (
        c.x - b.x,
        c.y - b.y
    )

    dot = (
        ab[0] * cb[0] +
        ab[1] * cb[1]
    )

    mag_ab = math.sqrt(
        ab[0] ** 2 +
        ab[1] ** 2
    )

    mag_cb = math.sqrt(
        cb[0] ** 2 +
        cb[1] ** 2
    )

    if mag_ab == 0 or mag_cb == 0:
        return 0

    cosine = dot / (mag_ab * mag_cb)

    cosine = max(
        -1.0,
        min(
            1.0,
            cosine
        )
    )

    return math.degrees(
        math.acos(cosine)
    )

# Calculate hand center using palm landmarks
def hand_center(hand):
    points = [
        hand[0],
        hand[5],
        hand[9],
        hand[13],
        hand[17]
    ]

    x = (
        sum(
            point.x
            for point in points
        )
        / len(points)
    )

    y = (
        sum(
            point.y
            for point in points
        )
        / len(points)
    )

    return x, y

# Closed fist means PAUSE
def is_fist(hand):
    return (
        hand[8].y > hand[6].y
        and
        hand[12].y > hand[10].y
        and
        hand[16].y > hand[14].y
        and
        hand[20].y > hand[18].y
    )

# Two fingers means ROTATE
def is_rotate(hand):
    index_open = (
        hand[8].y < hand[6].y
    )

    middle_open = (
        hand[12].y < hand[10].y
    )

    ring_closed = (
        hand[16].y > hand[14].y
    )

    pinky_closed = (
        hand[20].y > hand[18].y
    )

    return (
        index_open
        and
        middle_open
        and
        ring_closed
        and
        pinky_closed
    )

# Check for a fully open hand
def is_open_hand(hand):
    four_fingers_open = (
        hand[8].y < hand[6].y
        and
        hand[12].y < hand[10].y
        and
        hand[16].y < hand[14].y
        and
        hand[20].y < hand[18].y
    )

    thumb_angle = angle(
        hand[2],
        hand[3],
        hand[4]
    )

    thumb_straight = (
        thumb_angle > 145
    )

    palm_width = distance(
        hand[5],
        hand[17]
    )

    thumb_from_palm = distance(
        hand[4],
        hand[9]
    )

    thumb_spread = (
        thumb_from_palm
        >
        palm_width * 0.90
    )

    return (
        four_fingers_open
        and
        thumb_straight
        and
        thumb_spread
    )

# Store recent hand positions for DROP detection
position_history = deque()

HISTORY_TIME = 0.40
DROP_DISTANCE = 0.16
DROP_COOLDOWN = 0.80

last_drop_time = 0

# Detect a quick downward hand movement
def detect_drop(hand, current_time):
    global last_drop_time

    x, y = hand_center(hand)

    position_history.append(
        (
            current_time,
            x,
            y
        )
    )

    # Remove old movement samples
    while (
        position_history
        and
        current_time - position_history[0][0]
        > HISTORY_TIME
    ):
        position_history.popleft()

    if len(position_history) < 4:
        return False

    if (
        current_time - last_drop_time
        < DROP_COOLDOWN
    ):
        return False

    old_time, old_x, old_y = (
        position_history[0]
    )

    dx = x - old_x
    dy = y - old_y

    # Camera Y increases downward
    moved_down = (
        dy > DROP_DISTANCE
    )

    # Ignore mostly horizontal movement
    mostly_vertical = (
        abs(dy)
        >
        abs(dx) * 1.3
    )

    if moved_down and mostly_vertical:
        last_drop_time = current_time

        position_history.clear()

        return True

    return False

# Determine the current static gesture
def get_static_pose(hand, hand_name):
    # Fist gets highest static priority
    if is_fist(hand):
        return "PAUSE"

    # Two fingers means ROTATE
    if is_rotate(hand):
        return "ROTATE"

    # Open hand controls LEFT or RIGHT
    if is_open_hand(hand):
        if hand_name == "Left":
            return "LEFT"

        if hand_name == "Right":
            return "RIGHT"

    return ""

# Open the camera
cap = cv2.VideoCapture(0)

if not cap.isOpened():
    print("ERROR: Camera could not be opened")

    sock.close()

    raise SystemExit

# Print controller information
print()
print("================================")
print(" GestureOS Tetris Controller")
print("================================")
print()
print("ESP32 IP:", ESP32_IP)
print("UDP Port:", ESP32_PORT)
print()
print("LEFT hand open   -> LEFT once")
print("RIGHT hand open  -> RIGHT once")
print("Two fingers      -> ROTATE once")
print("Closed fist      -> PAUSE once")
print("Quick move DOWN  -> DROP once")
print()
print("Holding a gesture does not repeat it.")
print()
print("Press Q to quit.")
print()

# Keep DROP text visible briefly
drop_display_until = 0

# Start MediaPipe hand detection
with HandLandmarker.create_from_options(
    options
) as landmarker:

    while True:
        success, frame = cap.read()

        if not success:
            print("Camera frame failed.")
            break

        # Mirror the camera
        frame = cv2.flip(
            frame,
            1
        )

        # Convert OpenCV image to RGB
        rgb_frame = cv2.cvtColor(
            frame,
            cv2.COLOR_BGR2RGB
        )

        # Create MediaPipe image
        mp_image = mp.Image(
            image_format=mp.ImageFormat.SRGB,
            data=rgb_frame
        )

        # MediaPipe VIDEO mode needs an increasing timestamp
        timestamp_ms = int(
            time.monotonic() * 1000
        )

        # Detect the hand
        result = landmarker.detect_for_video(
            mp_image,
            timestamp_ms
        )

        current_time = time.monotonic()

        display_gesture = ""

        # Process detected hand
        if result.hand_landmarks:
            hand = result.hand_landmarks[0]

            # Get MediaPipe handedness
            detected_hand = (
                result
                .handedness[0][0]
                .category_name
            )

            # Correct handedness because the camera is mirrored
            if detected_hand == "Left":
                hand_name = "Right"
            else:
                hand_name = "Left"

            height, width, _ = frame.shape

            # Draw all 21 hand landmarks
            for landmark in hand:
                px = int(
                    landmark.x * width
                )

                py = int(
                    landmark.y * height
                )

                cv2.circle(
                    frame,
                    (
                        px,
                        py
                    ),
                    5,
                    (
                        0,
                        255,
                        0
                    ),
                    -1
                )

            # Show detected real hand
            cv2.putText(
                frame,
                "Hand: " + hand_name,
                (
                    30,
                    110
                ),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.8,
                (
                    255,
                    255,
                    255
                ),
                2
            )

            # DROP has highest priority
            if detect_drop(
                hand,
                current_time
            ):
                send_command(
                    "DROP"
                )

                display_gesture = "DROP"

                drop_display_until = (
                    current_time + 0.5
                )

                # Require neutral after DROP
                drop_static_lock = True

            else:
                # Detect the current static gesture
                static_pose = get_static_pose(
                    hand,
                    hand_name
                )

                if static_pose:
                    display_gesture = static_pose

                    # Static gestures remain blocked until neutral after DROP
                    if not drop_static_lock:
                        register_static_pose(
                            static_pose
                        )

                else:
                    # Neutral hand position re-arms gestures
                    register_neutral(
                        current_time
                    )

        else:
            # Clear DROP history when the hand disappears
            position_history.clear()

            # Re-arm all static gestures
            register_no_hand()

        # Keep DROP visible briefly without sending it again
        if (
            current_time
            <
            drop_display_until
        ):
            display_gesture = "DROP"

        # Show the detected gesture
        if display_gesture:
            cv2.putText(
                frame,
                display_gesture,
                (
                    30,
                    60
                ),
                cv2.FONT_HERSHEY_SIMPLEX,
                1.5,
                (
                    0,
                    0,
                    255
                ),
                3
            )

        # Show camera window
        cv2.imshow(
            "GestureOS WiFi Control",
            frame
        )

        # Press Q to exit
        key = cv2.waitKey(1) & 0xFF

        if key == ord("q"):
            break

# Release camera and network resources
cap.release()
cv2.destroyAllWindows()
sock.close()

print("Camera stopped.")
