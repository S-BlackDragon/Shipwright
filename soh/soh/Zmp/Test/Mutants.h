#pragma once

// Mutation test of the test suite (docs/DECISIONES.md D-079): bugs this project already had, brought back on purpose,
// one at a time, to check that the suite still catches them. A mutant exists only in the test build (ZMP_HARNESS):
// in the demo Zmp_TestMutant() is the constant 0 and the mutated branches are dead code. It is chosen with the
// environment variable ZMP_MUTANT (the suite's --mutante) or the CVar gZmp.Test.Mutant.
//
//   estado_texto           a cutscene's text statics do not travel in the group state (D-065): desync on entering
//                          during a cutscene's text
//   estado_efectos         the effects' search index does not travel in the group state (D-044)
//   sabotaje_actor         one machine changes an actor outside the simulation (what the resync test does by hand)
//   robo_foco              a test window takes the PC's focus when it opens (D-063); it gives it back at once
//   sonido_sin_foco        a window without the real focus plays sound when a test says it has the focus (D-063)
//   congelon_entrar        who is playing freezes when somebody enters its group (D-070)
//   barras_entrar          black bars and no HUD after a group cutscene for who entered during it (D-059)
//   guardado_no_anfitrion  a PC that is not the host writes its save file (D-058)
//   cierre_casa            the group state is loaded in the middle of the player's own scene change (D-064: the game
//                          closed leaving Link's house into an occupied forest)
//   estado_temblor         the quakes in progress do not travel in the group state (D-081): desync on entering
//                          during a quake
//   camara_heredada        a Link that appears keeps the camera mode of the player who was there (D-080): the game
//                          reads past a camera table in rooms with a fixed background
//   cuadro_crash           a test instance that crashes keeps the game's "Crash" dialog on the screen and never
//                          ends by itself

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ZMP_HARNESS
int Zmp_TestMutant(const char* name);
// robo_foco: called at the very start of main() and once per frame.
void Zmp_TestFocusMutantStart(void);
void Zmp_TestFocusMutantFrame(void);
#else
static inline int Zmp_TestMutant(const char* name) {
    (void)name;
    return 0;
}
static inline void Zmp_TestFocusMutantStart(void) {
}
static inline void Zmp_TestFocusMutantFrame(void) {
}
#endif

#ifdef __cplusplus
}
#endif
