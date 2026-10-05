#include "../src/proxy/GameState.cpp"
#include <cstdio>
int main(){
 using namespace arvr;
 SetControllerMenu(false);MarkCameraGameplay(true);
 if(ControllerMenuActive()||!GameplayFirstPersonActive())return 1;
 SetControllerMenu(true);MarkCameraGameplay(false);MarkCameraGameplay(true);
 if(!ControllerMenuActive()||CurrentSceneMode()!=SceneFlatScreen)return 2;
 SetControllerMenu(false);
 if(CurrentSceneMode()!=SceneGameplayVR)return 3;
 puts("PASS: camera updates do not override actual pause; native unpause restores gameplay without controller toggle");
}
