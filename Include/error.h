/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 *  error.h
 *  Return statuses
*/

#ifndef __ERROR_H__
#define __ERROR_H__

#define E_OK        ( (ER) 0 )                  /* Operation OK             */
#define E_NOMEM     ( (ER)-10 )                 /* No memory error          */
#define E_RSFN      ( (ER)-20 )                 /* Illegal service          */
#define E_PAR       ( (ER)-33 )                 /* Parameter error          */
#define E_ID        ( (ER)-35 )                 /* Illegal ID               */
#define E_NOEXS     ( (ER)-52 )                 /* Structure not exists     */
#define E_OBJ       ( (ER)-63 )                 /* Target proc is dormant   */
#define E_QOVR      ( (ER)-73 )                 /* Structure param overrun  */
#define E_TMOUT     ( (ER)-85 )                 /* Service timeout ocurred  */
#define E_RLWAI     ( (ER)-86 )                 /* Release wait happened    */
#define E_SYS1		( (ER)-90 )					// system specific error #1
#define E_SYS2		( (ER)-91 )					// system specific error #2 
#define E_ERROR		( (ER) 1 )					// error in operation

#endif  /* __ERROR_H__  */
