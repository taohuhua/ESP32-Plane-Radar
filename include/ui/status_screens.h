#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();

/** Blocking radar-sweep transition, played on a range/location button press. */
void statusScreenRadarSweep(const char* label);
