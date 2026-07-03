// Stub minimal : la plateforme CUSTOM n'utilise pas l'émulation souris quadrature
// (uni_mouse_quadrature.c retiré car il dépend de driver/timer.h, API legacy supprimée
// en IDF 6.1). La console BT y référence seulement le facteur d'échelle : on le stube.
#include "uni_mouse_quadrature.h"

static float s_scale = 1.0f;

void uni_mouse_quadrature_set_scale_factor(float scale) { s_scale = scale; }
float uni_mouse_quadrature_get_scale_factor(void) { return s_scale; }
