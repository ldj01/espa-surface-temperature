;
; This is based on the original RIT prototype, with the follwoign changes:
;
; - L5v2.rsp is not available, so we are using our equivalent files.  This is
;   needed for comparison anyway.
; - Debug messages are added.
; - Our directory structure is different, so the code is adjusted for that.
; - Our MODTRAN version is different, so we scan MODTRAN files for different 
;   strings.
;
; Monica Cook
; 21 January 2014
;
;
; NAME:  lst_merra_step2
;  
; PURPOSE:  IDL PROCEDURE
;   Generate transmission, upwelled radiance, and downwelled radiance at each height at each NARR point
;
; CALL SEQUENCE: lst_merra_step2, home, $            ;directory containing programs and supporting files
;                                 directory, $       ;directory containing landsat metadata and location for results
;                              	  numPoints, $       ;number of NARR points within Landsat scene
;                                 numHeights, $      ;number of height at each NARR point
;                                 alb, $             ;albedo of second MODTRAN run at each height at each NARR point
;			          whichLandsat	  ;which Landsat sensors this image is captured with
;
;
;  RESTRICTIONS:
;    1) Names of RSR files are hard coded, and are assumed to be in the home directory
;    2) The lat/lons and height for a given reanalysis point are grabbed from the path strings in the caseList file.
;       The folder heirarchy rarely changes so the code looks for strings between path seperators ('/'), and the position
;       that is expects to find lat/lon and height are hard coded in. They should be changed if the folder heirarchy will
;       change the position where the lat/lon and height folders are.
;    
;  
;  REQUIRED PROGRAMS AND FILES (in home directory):   L5v2.rsp
;                                                     caseList
;                                                     CALCULATE_LT.pro
;                                                     CALCULATE_LOBS.pro
;
;
;  MODIFICATIONS
;    Jan 2014     Original code
;    Feb 2017     Changed hard-coded line numbers for RSR to using FILE_LINES to get # of lines    
;

PRO lst_merra_step2, home, $
                     directory, $
                     numPoints, $
                     numHeights, $
                     alb, $
                     whichLandsat

   ;convert inputs
   home = STRING(home)
   directory = STRING(directory)
   numPoints = FIX(numPoints)
   numHeights = FIX(numHeights)
   alb = DOUBLE(alb)
   whichLandsat = FIX(whichLandsat)
   
   ;define emissivity from albedo
   ems = 1-alb

   PRINT, "home: ", home
   PRINT, "directory: ", directory
   PRINT, "numPoints: ", numPoints
   PRINT, "numHeights: ", numHeights
   PRINT, "alb: ", alb
   PRINT, "whichLandsat: ", whichLandsat
   PRINT, "ems: ", ems
   
   CASE whichLandsat OF
      5: BEGIN
         ; We don't have L5v2.rsp so use L5.rsp that we supply."
         ;nlines = FILE_LINES(home+'L5v2.rsp')
         ;OPENR, 20, home+'L5v2.rsp'
         nlines = FILE_LINES(home+'L5.rsp')
         OPENR, 20, home+'L5.rsp'
         spectralResponse = MAKE_ARRAY(2, nlines, /DOUBLE)
         READF, 20, spectralResponse
         CLOSE, 20
         FREE_LUN, 20

         PRINT, "nlines", nlines
         PRINT, "spectralResponse", spectralResponse

      END
      7: BEGIN
	nlines = FILE_LINES(home+'L7.rsp')
         OPENR, 20, home+'L7.rsp'
         spectralResponse = MAKE_ARRAY(2, nlines, /DOUBLE)
         READF, 20, spectralResponse
         CLOSE, 20
         FREE_LUN, 20
      END
      10: BEGIN
         nlines = FILE_LINES(home+'L8_B10.rsp')
         OPENR, 20, home+'L8_B10.rsp'
         spectralResponse = MAKE_ARRAY(2, nlines, /DOUBLE)
         READF, 20, spectralResponse
         CLOSE, 20
         FREE_LUN, 20
      END
      11: BEGIN
         nlines = FILE_LINES(home+'L8_B11.rsp') 
         OPENR, 20, home+'L8_B11.rsp'
         spectralResponse = MAKE_ARRAY(2, nlines, /DOUBLE)
         READF, 20, spectralResponse
         CLOSE, 20
         FREE_LUN, 20
      END
   ENDCASE  
   
   ;read caseList into string array
   caseListFile = home+directory+'caseList'
   ;determine number of entries in caseList file
   command = "wc "+caseListFile+" | awk '{print $1}'"
   SPAWN, command, numEntries
   numEntries = FIX(numEntries[0])   

   PRINT, "caseListFile:", caseListFile
   PRINT, "command:", command
   PRINT, "numEntries:", numEntries
   
   OPENR, 20, caseListFile
   caseList = MAKE_ARRAY(1, numEntries, /STRING)
   READF, 20, caseList
   CLOSE, 20
   FREE_LUN, 20

   PRINT, "caseList:", caseList
   
   ;calculate Lt for each temperature
   tempRadiance273 = CALCULATE_LT(273, spectralResponse)
   tempRadiance310 = CALCULATE_LT(310, spectralResponse)

   PRINT, "tempRadiance273:", tempRadiance273
   PRINT, "tempRadiance310:", tempRadiance310
 
   ;initialize counter
   ;counter is where to extract from caseList
   counter = 0      
   
   ;initialize place
   ;place is where to place entries in results
   place = 0
   
   ;create array to contain calculated output
   results = MAKE_ARRAY(6, numPoints*numHeights, /STRING)

   PRINT, "results:", results
   
   ;iterate through all points in the scene
   FOR i = 0, numPoints-1 DO BEGIN

      PRINT, "point i:", i
     
      ;iterate through all heights at each points
      FOR j = 0, numHeights-1 DO BEGIN

         PRINT, "height j:", j
      
         ; determine current latlon and height
         ; depends on number of steps in path
         ; these positions may need to be changed
         extract = caseList[counter]
         command = "echo "+extract+" | tr '/' '\n'"

         PRINT, "extract:", extract
         PRINT, "counter:", counter
         PRINT, "command:", command

         SPAWN, command, specs

         ;PRINT, "specs:", specs
         ;PRINT, "specs[0]:", specs[0]
         ;PRINT, "specs[1]:", specs[1]
         ;PRINT, "specs[2]:", specs[2]
         ;PRINT, "specs[3]:", specs[3]
         ;PRINT, "specs[4]:", specs[4]
         ;PRINT, "specs[5]:", specs[5]
         ;PRINT, "specs[6]:", specs[6]
         ;PRINT, "specs[7]:", specs[7]
         ;PRINT, "specs[8]:", specs[8]

         ; Our directory structure is a bit different from RIT's.
         ; lat_lon = specs[6]
         ; height = specs[7]
         lat_lon = specs[8]
         height = specs[9]

         PRINT, "lat_lon:", lat_lon
         PRINT, "height:", height

         command = "echo "+lat_lon+" | tr '_' '\n'"

         PRINT, "command:", command

         SPAWN, command, coordinates

         PRINT, "coordinates:", coordinates

         lat = coordinates[0]
         PRINT, "lat:", lat
         lon = coordinates[1]
         PRINT, "lon:", lon
         
         ;determine number of entries in current file
         command = "wc "+extract+"/parsed | awk '{print $1}'"
         SPAWN, command, numEntries
         numEntries = FIX(numEntries[0])

         PRINT, "command:", command
         PRINT, "numEntries:", numEntries
               
         ;for each height, read in radiance inforomation for three modtran runs
         ;columns of array are organized:
         ;wavelength | 273,0.0 | 310,0.0 | 000,0.1
         currentData = MAKE_ARRAY(4, numEntries, /DOUBLE)

         PRINT, "currentData:", currentData
         
         ;initialize index
         ;index is where to put data from parsed file into current data
         index = 0
      
         ;iterature through three pairs of parameters
         FOR k = 0, 2 DO BEGIN

            PRINT, "index:", index
            PRINT, "k:", k
            
            ;define current file
            currentFile = caseList[counter]+'/parsed'

            PRINT, "currentFile:", currentFile

            OPENR, 20, currentFile
            temp = MAKE_ARRAY(2, numEntries, /DOUBLE)
            READF, 20, temp
            CLOSE, 20
            FREE_LUN, 20

            PRINT, "temp:", temp
               
            ;put arrays into data array for current point at current height
            IF index EQ 0 THEN BEGIN
               currentData[0,*] = temp[0,*]
               index = index + 1
            ENDIF
               
            currentData[index,*] = temp[1,*]
            
            IF k EQ 2 THEN BEGIN
               ;determine temperature at lowest atmospheric layer (when MODTRAN is run at 0K)
               zeroTape6 = caseList[counter]+'/tape6'
               ; We have a different format due to different MODTRAN versions.
               ;command = 'grep "TARGET-PIXEL (H2) SURFACE TEMPERATURE" '+ zeroTape6 + " | awk '{print $7}'"
               command = 'grep "AREA-AVERAGED GROUND TEMPERATURE \[K\]" '+ zeroTape6 + " | awk '{print $6}'"
               SPAWN, command, zeroTemp
               zeroTemp = DOUBLE(zeroTemp[0])
            ENDIF               
            
            ;iterate counter and index
            counter = counter+1
            index = index+1 
                                                   
         ENDFOR
                                       
         ;extract wavelengths from array of current data
         wavelengths = currentData[0,*]
         
         ;***parameters from 3 modtran runs
                                   
         ; Lobs = Lt*tau + Lu
         ; m = tau
         ; b = Lu
         x = MAKE_ARRAY(2,2)
         x[0,*] = 1
         x[1,0] = tempRadiance273
         x[1,1] = tempRadiance310
         y = MAKE_ARRAY(1,2)
         y[0,0] = CALCULATE_LOBS(wavelengths, currentData[1,*], spectralResponse)
         y[0,1] = CALCULATE_LOBS(wavelengths, currentData[2,*], spectralResponse)
         a = INVERT(TRANSPOSE(x)##x)##TRANSPOSE(x)##y
         tau = a[1]
         Lu = a[0]
      
         ;determine Lobs and Lt when modtran was run a 0 K - calculate downwelled
         tempRadianceZero = CALCULATE_LT(zeroTemp, spectralResponse)
         obsRadianceZero = CALCULATE_LOBS(wavelengths, currentData[3,*], spectralResponse)
         ;Ld = (((Lobs-Lu)/tau) - (Lt*ems))/(1-ems)
         Ld = (((obsRadianceZero-Lu)/tau)-(tempRadianceZero*ems))/(1-ems)         
         PRINT, "lat_lon height obsRadianceZero tempRadianceZero Lu tau Ld ems:", lat_lon, " ", height, " ", obsRadianceZero, " ", tempRadianceZero, " ", Lu, " ", tau, " ", Ld, " ", ems
            
         ;convert results to strings
         tau = STRING(tau)
         Lu = STRING(Lu)
         Ld = STRING(Ld)
         
         ;put results into results array
         results[*,place] = [lat, lon, height, tau, Lu, Ld]

         ;iterate place
         place = place+1
         
              
      ;end for loop iterating heights                                       
      ENDFOR
   ;end for loop iterating narr points   
   ENDFOR

   PRINT, "FINISHED POINTS"
   
   ;determine file to output results to
   file = home+directory+'atmosphericParameters.txt'
   PRINT, file
   HELP, results
   PRINT, results
   
   ;write results to a file
   OPENW, unit, file, /GET_LUN
   PRINTF, unit, results
   CLOSE, unit
   FREE_LUN, unit

   PRINT, "WROTE FILE"


END
