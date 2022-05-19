#include "displayapp/screens/Notifications.h"
#include "displayapp/DisplayApp.h"
#include "components/ble/MusicService.h"
#include "components/ble/AlertNotificationService.h"
#include "displayapp/screens/Symbols.h"
#include <algorithm>

using namespace Pinetime::Applications::Screens;
extern lv_font_t jetbrains_mono_extrabold_compressed;
extern lv_font_t jetbrains_mono_bold_20;

Notifications::Notifications(DisplayApp* app,
                             Pinetime::Controllers::NotificationManager& notificationManager,
                             Pinetime::Controllers::AlertNotificationService& alertNotificationService,
                             Pinetime::Controllers::MotorController& motorController,
                             System::SystemTask& systemTask,
                             Modes mode)
  : Screen(app),
    notificationManager {notificationManager},
    alertNotificationService {alertNotificationService},
    motorController {motorController},
    systemTask {systemTask},
    mode {mode} {

  notificationManager.ClearNewNotificationFlag();
  auto notification = notificationManager.GetLastNotification();
  currentId = 0;
  if (notification.valid) {
    currentItem = std::make_unique<NotificationItem>(notification.Title(),
                                                     notification.Message(),
                                                     1,
                                                     notification.category,
                                                     notificationManager.NbNotifications(),
                                                     mode,
                                                     alertNotificationService,
                                                     motorController);
    validDisplay = true;
  } else {
    currentItem = std::make_unique<NotificationItem>(notification.category, alertNotificationService, motorController);
  }

  if (mode == Modes::Preview) {
    systemTask.PushMessage(System::Messages::DisableSleeping);
    if (notification.category == Controllers::NotificationManager::Categories::IncomingCall) {
      motorController.StartRinging();
    } else {
      motorController.RunForDuration(35);
    }

    timeoutLine = lv_line_create(lv_scr_act(), nullptr);

    lv_obj_set_style_local_line_width(timeoutLine, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, 3);
    lv_obj_set_style_local_line_color(timeoutLine, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_WHITE);
    lv_obj_set_style_local_line_rounded(timeoutLine, LV_LINE_PART_MAIN, LV_STATE_DEFAULT, true);

    lv_line_set_points(timeoutLine, timeoutLinePoints, 2);
    timeoutTickCountStart = xTaskGetTickCount();
    interacted = false;
  }

  taskRefresh = lv_task_create(RefreshTaskCallback, LV_DISP_DEF_REFR_PERIOD, LV_TASK_PRIO_MID, this);
}

Notifications::~Notifications() {
  lv_task_del(taskRefresh);
  // make sure we stop any vibrations before exiting
  motorController.StopRinging();
  systemTask.PushMessage(System::Messages::EnableSleeping);
  lv_obj_clean(lv_scr_act());
}

void Notifications::Refresh() {
  if (mode == Modes::Preview && timeoutLine != nullptr) {
    TickType_t tick = xTaskGetTickCount();
    int32_t pos = LV_HOR_RES - ((tick - timeoutTickCountStart) / (timeoutLength / LV_HOR_RES));
    if (pos <= 0) {
      running = false;
    } else {
      timeoutLinePoints[1].x = pos;
      lv_line_set_points(timeoutLine, timeoutLinePoints, 2);
    }
  }

  if (currentItem != nullptr && currentItem->AnimationElapsed()) {
    auto notification = notificationManager.At(currentId);
    if (!notification.valid) {
      notification = notificationManager.GetLastNotification();
      currentId = 0;
    }

    if (!notification.valid) {
      validDisplay = false;
    }

    currentItem.reset(nullptr);
    app->SetFullRefresh(DisplayApp::FullRefreshDirections::Up);

    if (validDisplay) {
      currentItem = std::make_unique<NotificationItem>(notification.Title(),
                                                       notification.Message(),
                                                       currentId + 1,
                                                       notification.category,
                                                       notificationManager.NbNotifications(),
                                                       mode,
                                                       alertNotificationService,
                                                       motorController);
    } else {
      currentItem = std::make_unique<NotificationItem>(notification.category, alertNotificationService, motorController);
      currentId = 0;
    }
  }

  running = currentItem->IsRunning() && running;
}

void Notifications::OnPreviewInteraction() {
  systemTask.PushMessage(System::Messages::EnableSleeping);
  motorController.StopRinging();
  if (timeoutLine != nullptr) {
    lv_obj_del(timeoutLine);
    timeoutLine = nullptr;
  }
}

bool Notifications::OnTouchEvent(Pinetime::Applications::TouchEvents event) {
  if (mode != Modes::Normal) {
    if (!interacted && event == TouchEvents::Tap) {
      interacted = true;
      OnPreviewInteraction();
      return true;
    }
    return false;
  }

  switch (event) {
    case Pinetime::Applications::TouchEvents::SwipeRight:
      if (validDisplay) {
        notificationManager.Dismiss(currentId);
        if (currentId > 0 && currentId == notificationManager.NbNotifications()) {
          // dismissed last message (like 5/5), need to go one message down (like 4/4)
          --currentId;
        }
        currentItem->AnimateDismiss();
        return true;
      }
      return false;
    case Pinetime::Applications::TouchEvents::SwipeDown: {
      Controllers::NotificationManager::Notification previousNotification;
      if (validDisplay) {
        previousNotification = notificationManager.GetPrevious(currentId);
        if (previousNotification.valid) {
          currentId = std::min(static_cast<uint8_t>(currentId + 1), static_cast<uint8_t>(notificationManager.NbNotifications() - 1));
        }
      } else {
        previousNotification = notificationManager.GetLastNotification();
        currentId = 0;
      }

      if (!previousNotification.valid) {
        return true;
      }

      validDisplay = true;
      currentItem.reset(nullptr);
      app->SetFullRefresh(DisplayApp::FullRefreshDirections::Down);
      currentItem = std::make_unique<NotificationItem>(previousNotification.Title(),
                                                       previousNotification.Message(),
                                                       currentId + 1,
                                                       previousNotification.category,
                                                       notificationManager.NbNotifications(),
                                                       mode,
                                                       alertNotificationService,
                                                       motorController);
    }
      return true;
    case Pinetime::Applications::TouchEvents::SwipeUp: {
      Controllers::NotificationManager::Notification nextNotification;
      if (validDisplay)
        nextNotification = notificationManager.GetNext(currentId);
      else
        nextNotification = notificationManager.GetLastNotification();

      if (!nextNotification.valid) {
        running = false;
        return false;
      }

      validDisplay = true;
      if (currentId > 0)
        --currentId;
      currentItem.reset(nullptr);
      app->SetFullRefresh(DisplayApp::FullRefreshDirections::Up);
      currentItem = std::make_unique<NotificationItem>(nextNotification.Title(),
                                                       nextNotification.Message(),
                                                       currentId + 1,
                                                       nextNotification.category,
                                                       notificationManager.NbNotifications(),
                                                       mode,
                                                       alertNotificationService,
                                                       motorController);
    }
      return true;
    default:
      return false;
  }
}

namespace {
  void CallEventHandler(lv_obj_t* obj, lv_event_t event) {
    auto* item = static_cast<Notifications::NotificationItem*>(obj->user_data);
    item->OnCallButtonEvent(obj, event);
  }
}

Notifications::NotificationItem::NotificationItem(Controllers::NotificationManager::Categories category,
                                                  Pinetime::Controllers::AlertNotificationService& alertNotificationService,
                                                  Pinetime::Controllers::MotorController& motorController)
  : NotificationItem("Notification",
                     "No notification to display",
                     0,
                     category,
                     0,
                     Modes::Preview,
                     alertNotificationService,
                     motorController) {
}

Notifications::NotificationItem::NotificationItem(const char* title,
                                                  const char* msg,
                                                  uint8_t notifNr,
                                                  Controllers::NotificationManager::Categories category,
                                                  uint8_t notifNb,
                                                  Modes mode,
                                                  Pinetime::Controllers::AlertNotificationService& alertNotificationService,
                                                  Pinetime::Controllers::MotorController& motorController)
  : mode {mode}, alertNotificationService {alertNotificationService}, motorController {motorController} {

  container = lv_cont_create(lv_scr_act(), nullptr);
  lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_local_bg_color(container, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_BLACK);
  lv_obj_set_style_local_pad_all(container, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, 0);
  lv_obj_set_style_local_pad_inner(container, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, 0);
  lv_obj_set_style_local_border_width(container, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, 0);

  subject_container = lv_cont_create(container, nullptr);
  lv_obj_set_style_local_bg_color(subject_container, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0x38, 0x38, 0x38));
  lv_obj_set_style_local_pad_all(subject_container, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, 10);
  lv_obj_set_style_local_pad_inner(subject_container, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, 5);
  lv_obj_set_style_local_border_width(subject_container, LV_CONT_PART_MAIN, LV_STATE_DEFAULT, 0);

  lv_obj_set_pos(subject_container, 0, 50);
  lv_obj_set_size(subject_container, LV_HOR_RES, LV_VER_RES - 50);
  lv_cont_set_layout(subject_container, LV_LAYOUT_COLUMN_LEFT);
  lv_cont_set_fit(subject_container, LV_FIT_NONE);

  lv_anim_init(&dismissAnim);
  lv_anim_set_exec_cb(&dismissAnim, (lv_anim_exec_xcb_t) lv_obj_set_x);
  lv_anim_set_var(&dismissAnim, container);
  lv_anim_set_time(&dismissAnim, dismissAnimLength);
  lv_anim_set_values(&dismissAnim, 0, LV_HOR_RES);

  lv_obj_t* alert_count = lv_label_create(container, nullptr);
  lv_label_set_text_fmt(alert_count, "%i/%i", notifNr, notifNb);
  lv_obj_align(alert_count, NULL, LV_ALIGN_IN_TOP_RIGHT, 0, 16);

  lv_obj_t* alert_type = lv_label_create(container, nullptr);
  lv_obj_set_style_local_text_color(alert_type, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0xb0, 0xb0, 0xb0));
  if (title == nullptr) {
    lv_label_set_text_static(alert_type, "Notification");
  } else {
    // copy title to label and replace newlines with spaces
    lv_label_set_text(alert_type, title);
    char* pchar = strchr(lv_label_get_text(alert_type), '\n');
    while (pchar != nullptr) {
      *pchar = ' ';
      pchar = strchr(pchar + 1, '\n');
    }
    lv_label_refr_text(alert_type);
  }
  lv_label_set_long_mode(alert_type, LV_LABEL_LONG_SROLL_CIRC);
  lv_obj_set_width(alert_type, 180);
  lv_obj_align(alert_type, NULL, LV_ALIGN_IN_TOP_LEFT, 0, 16);

  /////////
  switch (category) {
    default: {
      lv_obj_t* alert_subject = lv_label_create(subject_container, nullptr);
      lv_obj_set_style_local_text_color(alert_subject, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0xff, 0xb0, 0x0));
      lv_label_set_long_mode(alert_subject, LV_LABEL_LONG_BREAK);
      lv_obj_set_width(alert_subject, LV_HOR_RES - 20);
      lv_label_set_text(alert_subject, msg);
    } break;
    case Controllers::NotificationManager::Categories::IncomingCall: {
      lv_obj_set_height(subject_container, 108);
      lv_obj_t* alert_subject = lv_label_create(subject_container, nullptr);
      lv_obj_set_style_local_text_color(alert_subject, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0xff, 0xb0, 0x0));
      lv_label_set_long_mode(alert_subject, LV_LABEL_LONG_BREAK);
      lv_obj_set_width(alert_subject, LV_HOR_RES - 20);
      lv_label_set_text_static(alert_subject, "Incoming call from");

      lv_obj_t* alert_caller = lv_label_create(subject_container, nullptr);
      lv_obj_align(alert_caller, alert_subject, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);
      lv_label_set_long_mode(alert_caller, LV_LABEL_LONG_BREAK);
      lv_obj_set_width(alert_caller, LV_HOR_RES - 20);
      lv_label_set_text(alert_caller, msg);

      bt_accept = lv_btn_create(container, nullptr);
      bt_accept->user_data = this;
      lv_obj_set_event_cb(bt_accept, CallEventHandler);
      lv_obj_set_size(bt_accept, 76, 76);
      lv_obj_align(bt_accept, NULL, LV_ALIGN_IN_BOTTOM_LEFT, 0, 0);
      label_accept = lv_label_create(bt_accept, nullptr);
      lv_label_set_text_static(label_accept, Symbols::phone);
      lv_obj_set_style_local_bg_color(bt_accept, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0x0, 0xb0, 0x0));

      bt_reject = lv_btn_create(container, nullptr);
      bt_reject->user_data = this;
      lv_obj_set_event_cb(bt_reject, CallEventHandler);
      lv_obj_set_size(bt_reject, 76, 76);
      lv_obj_align(bt_reject, NULL, LV_ALIGN_IN_BOTTOM_MID, 0, 0);
      label_reject = lv_label_create(bt_reject, nullptr);
      lv_label_set_text_static(label_reject, Symbols::phoneSlash);
      lv_obj_set_style_local_bg_color(bt_reject, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_RED);

      bt_mute = lv_btn_create(container, nullptr);
      bt_mute->user_data = this;
      lv_obj_set_event_cb(bt_mute, CallEventHandler);
      lv_obj_set_size(bt_mute, 76, 76);
      lv_obj_align(bt_mute, NULL, LV_ALIGN_IN_BOTTOM_RIGHT, 0, 0);
      label_mute = lv_label_create(bt_mute, nullptr);
      lv_label_set_text_static(label_mute, Symbols::volumMute);
      lv_obj_set_style_local_bg_color(bt_mute, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, LV_COLOR_MAKE(0xb0, 0xb0, 0xb0));
    } break;
  }
}

void Notifications::NotificationItem::OnCallButtonEvent(lv_obj_t* obj, lv_event_t event) {
  if (event != LV_EVENT_CLICKED) {
    return;
  }

  motorController.StopRinging();

  if (obj == bt_accept) {
    alertNotificationService.AcceptIncomingCall();
  } else if (obj == bt_reject) {
    alertNotificationService.RejectIncomingCall();
  } else if (obj == bt_mute) {
    alertNotificationService.MuteIncomingCall();
  }

  running = false;
}

void Notifications::NotificationItem::AnimateDismiss() {
  dismissAnimTickCount = xTaskGetTickCount();
  lv_anim_start(&dismissAnim);
}

bool Notifications::NotificationItem::AnimationElapsed() {
  bool elapsed = dismissAnimTickCount != 0 && xTaskGetTickCount() > dismissAnimTickCount + dismissAnimLength;

  if (elapsed)
    dismissAnimTickCount = 0;

  return elapsed;
}

Notifications::NotificationItem::~NotificationItem() {
  lv_obj_clean(lv_scr_act());
}
