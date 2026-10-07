/**
 ******************************************************************************
 * @file    cdc_tx.h
 * @brief   Envoi de chaines ASCII sur l'USB CDC (helper partage).
 ******************************************************************************
 */
#ifndef CDC_TX_H
#define CDC_TX_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Envoie une chaine ASCII (terminee '\0') sur l'USB CDC.
 *        Copie dans un buffer statique et attend la fin du TX precedent :
 *        CDC_Transmit_FS ne copie pas le buffer fourni.
 *        Non bloquant au-dela de ~5 ms ; jette si le lien est mort.
 */
void CDC_SendStr(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* CDC_TX_H */
