/***************************************************************************\
    Weather.cpp
    Miro "Jammer" Torrielli
 20Nov03

 - And then there was light
\***************************************************************************/

#include "cmpglobl.h"
#include "campcell.h"
#include "campterr.h"
#include "weather.h"
#include "math.h"
#include "f4find.h"
#include "entity.h"
#include "campaign.h"
#include "falcmesg.h"
#include "aiinput.h"
#include "f4thread.h"
#include "cmpclass.h"
#include "msginc/weathermsg.h"
#include "falcsess.h"
#include "f4comms.h"
#include "otwdrive.h"
#include "tmap.h"
#include "fakerand.h"
#include "find.h"
#include <string.h>
#include "fflog.h"


extern int gCurrentDataVersion;

// Artscout - 2026 (FRONTS)
extern int g_nWeatherFronts;
extern float g_fWeatherFrontsPerDay;
extern float g_fWeatherNoise;

static const char FRONTS_MAGIC[4] = {'F', 'R', 'N', 'T'};
// Where the fronts block sits in a Cobra-layout .wth: after the 29 bytes of
// weather and the two zero map dimensions Save has always written.
static const long FRONTS_OFFSET = 37;

static inline float Rand01()
{
    return (float)(rand() % 10000) / 10000.f;
}

static inline float RandIn(float a, float b)
{
    return a + (b - a) * Rand01();
}
float COVersion = 0.0077f; // Cobra file version kludge

WeatherClass::WeatherClass() : RealWeather()
{
    cumulusZ = stratusZ = stratus2Z = 0.f;
    cumulusBase = stratusBase = stratus2Base = 0;
    temperature = windSpeed = windHeading = turbFactor = 0.f;
    weatherCondition = 1;
    needsWeatherRefresh = updateLighting = lockedCondition =
        unlockableCondition = FALSE;
}

WeatherClass::~WeatherClass()
{
}

void WeatherClass::Init(bool instantAction)
{
    condCounter = 0;
    lastCheck = Camp_GetCurrentTime();
    weatherDay = TheCampaign.GetCurrentDay();


    if (not instantAction)
    {
        lockedCondition = TRUE;
        // Cobra - no random weather
        // UpdateCondition(min(1+rand()%4,4));
        UpdateCondition(PlayerOptions.weatherCondition);
    }

    switch (TimeOfDayGeneral())
    {
    case TOD_NIGHT:
    {
        windSpeed = (float)(windMin + (rand() % 5));
        temperature = (float)(tempMin + (rand() % 3));
        break;
    }

    case TOD_DAWNDUSK:
    {
        windSpeed = (float)(windMin + (rand() % 10));
        temperature = (float)(tempMin + (rand() % 5));
        break;
    }

    default:
    {
        windSpeed = (float)(windMed + (rand() % 20));
        temperature = (float)(min(tempMed + (rand() % 10), tempMax));
    }
    }


    windHeading = (rand() % 360) * DTR;

    if (windHeading > 2.f * PI)
        windHeading -= 2.f * PI;

    if (weatherCondition == INCLEMENT)
    {
        temperature *= 0.75f;
        windSpeed = windSpeed + (rand() % 20);
    }

    contrailLow = 100.f * (float)contrailBase + 100 * (rand() % 10);
    contrailHigh =
        max(95000.f, (float)(2 + contrailLow +
                             1000 * (rand() % 8))); // FRB - For the SR-71
    //contrailHigh = max(35000.f, (float)(2 + contrailLow + 1000 * (rand()%8)));

    cumulusZ = (float)-(100 * cumulusBase + 100 * (rand() % 5));

    if (weatherCondition > FAIR)
    {
        stratusZ = (float)-(100 * stratusBase + 100 * (rand() % 20));

        if (weatherCondition == INCLEMENT)
            stratusZ = (float)-(5000 + 100 * (rand() % 150));
    }
    else
    {
        stratusZ = (float)-(100 * stratusBase + 100 * (rand() % 30));
    }

    stratus2Z = (float)-(100 * stratus2Base + 100 * (rand() % 10));
    stratusDepth = 1000.0f + 100 * (rand() % 30);
    stratus2Z = (float)-stratus2Base * 100.0f;

    ShadingFactor = 0;

    // Artscout - 2026 (FRONTS): Init is also what Setup calls when the player
    // picks a condition, so it keeps the fronts it has and only moves the
    // prevailing condition under them. CampLoad seeds new ones and says
    // whether this game has fronts at all.
    frontMap.s.prevailing = (float)weatherCondition;

    GenerateClouds();
}

// The condition the player or a file asks for. With fronts on, that is the
// prevailing condition, and the renderer then resamples the local one.
void WeatherClass::UpdateCondition(int condition, bool bForce)
{
    frontMap.s.prevailing = (float)condition;
    ApplyCondition(condition, bForce, true);
}

void WeatherClass::ApplyCondition(int condition, bool bForce, bool bRefresh)
{
    weatherCondition = condition;

    if (weatherCondition not_eq oldWeatherCondition or bForce)
    {
        oldWeatherCondition = weatherCondition;
        updateLighting = TRUE;

        if (bRefresh)
            needsWeatherRefresh = TRUE;

        switch (weatherCondition)
        {
        case SUNNY:
        {
            tempMin = 12;
            tempMed = 18;
            tempMax = 30;
            windMin = 0;
            windMed = 5;
            windMax = 10;

            wHdgThresh = 99;
            stratusBase = 220;
            stratus2Base = 350;
            stratusDepth = 2000.0f;
            contrailBase = 340;
            turbFactor = 0.1f;
            break;
        }

        case FAIR:
        {
            tempMin = 12;
            tempMed = 18;
            tempMax = 28;
            windMin = 5;
            windMed = 10;
            windMax = 20;

            wHdgThresh = 98;
            stratusBase = 220;
            stratus2Base = 350;
            stratusDepth = 2000.0f;
            cumulusBase = 80;
            contrailBase = 280;
            turbFactor = 0.2f;
            break;
        }

        case POOR:
        {
            tempMin = 10;
            tempMed = 15;
            tempMax = 25;
            windMin = 10;
            windMed = 15;
            windMax = 25;

            wHdgThresh = 97;
            stratusBase = 150;
            stratus2Base = 350;
            stratusDepth = 2000.0f;
            contrailBase = 250;
            turbFactor = 0.3f;
            break;
        }

        case INCLEMENT:
        {
            tempMin = 9;
            tempMed = 14;
            tempMax = 22;
            windMin = 15;
            windMed = 25;
            windMax = 35;

            wHdgThresh = 96;
            stratusBase = 100;
            stratus2Base = 350;
            stratusDepth = 3000.0f;
            contrailBase = 200;
            turbFactor = 0.4f;
        }
        }
    }
}

void WeatherClass::UpdateWeather()
{
    if (not TheCampaign.IsMaster())
        return;

    float seed, delta;
    CampaignTime time, tDelta;

    if (weatherDay not_eq TheCampaign.GetCurrentDay())
    {
        switch (TimeOfDayGeneral())
        {
        case TOD_NIGHT:
        {
            windSpeed = (float)(windMin + rand() % 5);
            temperature = (float)(tempMin + rand() % 3);
            break;
        }

        case TOD_DAWNDUSK:
        {
            windSpeed = (float)(windMin + rand() % 10);
            temperature = (float)(tempMin + rand() % 5);
            break;
        }

        default:
        {
            windSpeed = (float)(windMed + rand() % 20);
            temperature = (float)(min(tempMed + rand() % 10, tempMax));
        }
        }

        windHeading = (rand() % 360) * DTR;

        if (windHeading > 2.f * PI)
            windHeading -= (2.f * PI);

        contrailLow = 100.f * (float)(contrailBase + 100 * rand() % 10);
        contrailHigh =
            max(95000.f, (float)(2 + contrailLow +
                                 1000 * (rand() % 8))); // FRB - For the SR-71
        //contrailHigh = max(35000.f, (float)(2+contrailLow+1000*rand()%8));

        cumulusZ = (float)-(100 * cumulusBase + 100 * rand() % 5);

        if (weatherCondition > FAIR)
            stratusZ = (float)-(100 * stratusBase + 100 * rand() % 5);
        else
            stratusZ = (float)-(100 * stratusBase + 100 * rand() % 10);

        stratus2Z = (float)-(100 * stratus2Base + 100 * rand() % 10);

        weatherDay = TheCampaign.GetCurrentDay();
    }

    time = Camp_GetCurrentTime();

    if (time - lastCheck > (CampaignMinutes / 2) or needsWeatherRefresh)
    {
        static int lastTOD = TOD_NIGHT;
        int h = FloatToInt32((windHeading - .5f * PI) * 3.f);
        FalconWeatherMessage *message = new FalconWeatherMessage(
            vuLocalSessionEntity->Id(), FalconLocalGame);

        CampaignTime gap = (time > lastCheck) ? time - lastCheck : 0;
        tDelta = (CampaignTime)max(min((time - lastCheck), 2 * CampaignMinutes),
                                   0.f);
        lastCheck = time;

        seed = (float)(rand() % 100) / 100.f;
        delta = (float)tDelta / (float)CampaignHours;

        switch (TimeOfDayGeneral())
        {
        case TOD_NIGHT:
        {
            if (temperature > tempMin)
                temperature -= (float)delta * seed * 2.f;
            else
                temperature += (float)delta * seed * 2.f;

            if (windSpeed > windMin)
                windSpeed -= (float)delta * seed * 4.f;
            else
                windSpeed += (float)delta * seed * 4.f;

            lastTOD = TOD_NIGHT;
            break;
        }

        case TOD_DAWNDUSK:
        {
            if (lastTOD == TOD_NIGHT)
            {
                if (temperature < tempMed - 2)
                    temperature += (float)delta * seed * 4.f;
                else
                    temperature -= (float)delta * seed * 4.f;

                if (windSpeed < windMed - 5)
                    windSpeed += (float)delta * seed;
                else
                    windSpeed -= (float)delta * seed;
            }
            else
            {
                if (temperature > tempMin + 4)
                    temperature -= (float)delta * seed * 4.f;
                else
                    temperature += (float)delta * seed * 4.f;

                if (windSpeed > windMin + 5)
                    windSpeed -= (float)delta * seed * 4.f;
                else
                    windSpeed += (float)delta * seed * 4.f;
            }

            break;
        }

        default:
        {
            if (temperature < tempMax)
                temperature += (float)delta * seed * 2.f;
            else
                temperature -= (float)delta * seed * 2.f;

            if (windSpeed < windMax)
                windSpeed += (float)delta * seed * 4.f;
            else
                windSpeed -= (float)delta * seed * 4.f;


            lastTOD = TOD_DAY;
        }
        }

        if (rand() % 100 > wHdgThresh or needsWeatherRefresh)
        {
            if (rand() % 8 > 4 + h)
                windHeading = windHeading + (float)delta / 3;
            else
                windHeading = windHeading - (float)delta / 3;

            if (windHeading < 0.f)
                windHeading += (float)(2.f * PI);

            if (windHeading > 2.f * PI)
                windHeading -= (float)(2.f * PI);
        }

        condCounter += rand() % 3;

        // Artscout - 2026 (FRONTS): the fronts are the change in the weather;
        // the old whole-map random walk would fight them.
        if (frontMap.active)
        {
            condCounter = 0;
            EvolveFronts(gap);
        }

        if (condCounter > 360)
        {
            condCounter = 0;
            int direction = rand() % 10;
            direction = (direction > 5) ? 1 : -1;

            switch (weatherCondition)
            {
            case SUNNY:
            {
                UpdateCondition(weatherCondition + 1);
                break;
            }

            case FAIR:
            {
                UpdateCondition(weatherCondition + direction);
                break;
            }

            case POOR:
            {
                UpdateCondition(weatherCondition + direction);
                break;
            }

            case INCLEMENT:
            {
                UpdateCondition(weatherCondition - 1);
            }
            }
        }

        FillMessage(message);
        FalconSendMessage(message, TRUE);
    }

    if (needsWeatherRefresh)
    {
        needsWeatherRefresh = FALSE;
    }


    // RED - Patch... as we use a single wind variable, at least do all needed calculations here for a vector
    mlTrig trigWind;
    mlSinCos(&trigWind, windHeading);
    WindVector.x = trigWind.cos * windSpeed;
    WindVector.y = trigWind.sin * windSpeed;
    WindVector.z = 0.0f;
}

void WeatherClass::SendWeather(VuTargetEntity *target)
{
    GenerateClouds(FALSE);

    FalconWeatherMessage *message;
    message = new FalconWeatherMessage(vuLocalSessionEntity->Id(), target);

    FillMessage(message);
    FalconSendMessage(message, TRUE);
}

void WeatherClass::FillMessage(FalconWeatherMessage *message)
{
    // With fronts the condition sent is the prevailing one: each machine works
    // out its own local condition from the fronts, where its viewer is.
    message->dataBlock.weatherCondition =
        frontMap.active ? WeatherFrontMap::Condition(frontMap.s.prevailing) :
                          weatherCondition;
    message->dataBlock.lastCheck = lastCheck;
    message->dataBlock.temperature = temperature;
    message->dataBlock.windSpeed = windSpeed;
    message->dataBlock.windHeading = windHeading;
    message->dataBlock.cumulusZ = cumulusZ;
    message->dataBlock.stratusZ = stratusZ;
    message->dataBlock.stratus2Z = stratus2Z;
    message->dataBlock.contrailLow = contrailLow;
    message->dataBlock.contrailHigh = contrailHigh;
    message->dataBlock.ShadingFactor = ShadingFactor;
    message->dataBlock.weatherQuality = WeatherQuality;
    message->dataBlock.frontsActive = frontMap.active ? 1 : 0;
    message->dataBlock.fronts = frontMap.s;
}

void WeatherClass::ReceiveWeather(FalconWeatherMessage *message)
{
    // Artscout - 2026 (FRONTS): the host's fronts replace ours; the local
    // condition, shading and weather quality come from them here, not from
    // wherever the host's viewer happens to be.
    bool fronts = message->dataBlock.frontsActive not_eq 0;
    frontMap.active = fronts;
    frontMap.s = message->dataBlock.fronts;

    if (not fronts)
        UpdateCondition(message->dataBlock.weatherCondition);

    lastCheck = message->dataBlock.lastCheck;
    temperature = message->dataBlock.temperature;
    windSpeed = message->dataBlock.windSpeed;
    windHeading = message->dataBlock.windHeading;
    cumulusZ = message->dataBlock.cumulusZ;
    stratusZ = message->dataBlock.stratusZ;
    stratus2Z = message->dataBlock.stratus2Z;
    contrailLow = message->dataBlock.contrailLow;
    contrailHigh = message->dataBlock.contrailHigh;

    if (not fronts)
    {
        ShadingFactor = message->dataBlock.ShadingFactor;
        WeatherQuality = message->dataBlock.weatherQuality;
    }

    if (TheCampaign.Flags bitand CAMP_NEED_WEATHER)
    {
        GenerateClouds(FALSE);
        TheCampaign.Flags and_eq compl CAMP_NEED_WEATHER;
    }

    TheCampaign.GotJoinData();
}

int WeatherClass::CampLoad(char *name, int type)
{
    char /* *data,*/ *data_ptr;
    BYTE utemp;
    float ftemp, ftemp1;

    Init((type == game_InstantAction or type == game_Dogfight));

    if (type == game_Campaign)
        unlockableCondition = TRUE;

    // Artscout - 2026 (FRONTS): a dogfight is set weather, so no fronts. Any
    // other game starts with some already on the map; if the file has its
    // own, ReadFronts below replaces these.
    frontMap.active = (g_nWeatherFronts not_eq 0) and type not_eq game_Dogfight;

    if (frontMap.active)
        SeedFronts(true);

    CampaignData cd = ReadCampFile(name, "wth");

    if (cd.dataSize == -1)
        return 0;

    data_ptr = cd.data;

    if (type not_eq game_Campaign and type not_eq game_PlayerPool)
    {
        if (gCampDataVersion >= 75)
        {
            UpdateCondition(*((int *)data_ptr));
            data_ptr += sizeof(int);

            lastCheck = *((CampaignTime *)data_ptr);
            data_ptr += sizeof(CampaignTime);

            temperature = *((float *)data_ptr);
            data_ptr += sizeof(float);

            windSpeed = *((float *)data_ptr);
            data_ptr += sizeof(float);

            windHeading = *((float *)data_ptr);
            data_ptr += sizeof(float);

            if (gCampDataVersion >= 76)
            {
                cumulusZ = *((float *)data_ptr);
                data_ptr += sizeof(float);
            }
            else
                cumulusZ = (float)-(100 * cumulusBase + 100 * rand() % 50);

            stratusZ = *((float *)data_ptr);
            data_ptr += sizeof(float);

            contrailLow = *((float *)data_ptr);
            data_ptr += sizeof(float);

            contrailHigh = *((float *)data_ptr);
            data_ptr += sizeof(float);
        }
        else
        {
            // Cobra - compatibility with Tacedit
            /* Tacedit label              Cobra Variables
            ====================     =========================
             Wind Direction         (4) Wind Heading (degrees)
             Wind Speed             (4) Cumulus Base (feet)
             Time                   (4) Campaign/TE Time
             Temperature            (4) Stratus Base (feet)
             Temp                   (1) Temperature (deg. Celcius)
             Wind                   (1) Wind Speed (knots)
             Cloud base             (1) Weather condition (1=Sunny,2=fair,3=poor,4=inclement)
             Con Layer Start        (1) Contrail Base (100's of feet)
             Con Layer End          (1) Overcast Depth (100's of feet)
             X Off                  (4) Stratus 2 Base (future)
             Y Off                  (4) Cobra file version (do not change) */

            // Cobra version check  (gCampDataVersion = 73 = SP3 version)
            // Tacedit reverses XOff and YOff when TE is saved. :^(
            if (*((float *)(data_ptr + 21)) == COVersion)
            {
                ftemp = *((float *)(data_ptr + 21));
                ftemp1 = *((float *)(data_ptr + 25));
            }
            else
            {
                ftemp = *((float *)(data_ptr + 25));
                ftemp1 = *((float *)(data_ptr + 21));
            }

            // if ((*((float *)(data_ptr+25))) == COVersion)
            if (ftemp == COVersion)
            {
                utemp = *((char *)(data_ptr + 18));
                UpdateCondition((int)utemp, false);

                windHeading = *((float *)data_ptr);
                data_ptr += sizeof(float);

                cumulusZ = *((float *)data_ptr);
                cumulusBase = (int)(cumulusZ / 100.f);
                cumulusZ = -cumulusZ;
                data_ptr += sizeof(float);

                lastCheck = *((CampaignTime *)data_ptr);
                data_ptr += sizeof(CampaignTime);

                stratusZ = *((float *)data_ptr);
                stratusBase = (int)(stratusZ / 100.f);
                stratusZ = -stratusZ;
                data_ptr += sizeof(float);

                utemp = *((char *)data_ptr);
                temperature = (float)(int)utemp;
                data_ptr += sizeof(char);

                utemp = *((char *)data_ptr);
                windSpeed = (float)utemp / (KPH_TO_FPS * FTPSEC_TO_KNOTS);
                data_ptr += sizeof(char);

                // utemp = *((char *)data_ptr);
                // UpdateCondition((int)utemp);
                data_ptr += sizeof(char);

                utemp = *((char *)data_ptr);
                contrailLow = (float)utemp * 1000.0f;
                contrailBase = (SLONG)(contrailLow / 100.f);
                contrailHigh = 95000.f;
                //contrailHigh = 45000.f;
                data_ptr += sizeof(char);

                utemp = *((char *)data_ptr);
                stratusDepth = (float)utemp * 100.0f;
                data_ptr += sizeof(char);

                stratus2Z = ftemp1;
                stratus2Base = (int)(stratus2Z / 100.f);
                stratus2Z = -stratus2Z;
            }
            else
            {
                // SUNNY=1  FAIR =2 POOR=3 INCLEMENT=4
                windHeading = *((float *)data_ptr);
                data_ptr += sizeof(float);
                windSpeed = *((float *)data_ptr);
                windSpeed = (float)windSpeed / (KPH_TO_FPS * FTPSEC_TO_KNOTS);
                data_ptr += sizeof(float);
                lastCheck = *((CampaignTime *)data_ptr);
                data_ptr += sizeof(CampaignTime);
                temperature = *((float *)data_ptr);
                data_ptr += sizeof(float);
                // TodaysTemp = *((uchar *) data_ptr);
                data_ptr += sizeof(uchar);
                // TodaysWind = *((uchar *) data_ptr);
                data_ptr += sizeof(uchar);
                cumulusBase = (int)*((uchar *)data_ptr);

                if (cumulusBase < 100)
                    cumulusBase = 100;

                cumulusZ = -(float)cumulusBase * 100.f;
                data_ptr += sizeof(uchar);
                contrailLow = (float)*((uchar *)data_ptr);
                contrailLow *= 1000.0f;
                contrailBase = (SLONG)(contrailLow / 100.0f);
                data_ptr += sizeof(uchar);
                contrailHigh = (float)*((uchar *)data_ptr);
                contrailHigh *= 1000.0f;

                if (contrailHigh < 95000.f)
                    contrailHigh = 95000.f;

                //if (contrailHigh < 45000.f)
                // contrailHigh = 45000.f;
                stratusBase = 220;
                stratus2Base = 350;
                stratusZ = -22000.f;
                stratus2Z = -35000.f;

                if (PlayerOptions.weatherCondition < 1 or
                    PlayerOptions.weatherCondition > 4)
                    PlayerOptions.weatherCondition = 1;

                UpdateCondition(PlayerOptions.weatherCondition, false);
                UpdateWeather();
            }
        }
    }

    ReadFronts(cd.data, cd.dataSize, type);

    delete cd.data;

    // RED - Update the weather condition
    realWeather->UpdateCondition();

    return TRUE;
}

int WeatherClass::Save(char *name)
{
    FILE *fp;
    UINT nw = 0, nh = 0;
    unsigned int w = 0, h = 0;

    if ((fp = OpenCampFile(name, "wth", "wb")) == NULL)
        return 0;

    if (gCurrentDataVersion >= 75)
    {
        fwrite(&weatherCondition, sizeof(int), 1, fp);
        fwrite(&lastCheck, sizeof(CampaignTime), 1, fp);
        fwrite(&temperature, sizeof(float), 1, fp);
        fwrite(&windSpeed, sizeof(float), 1, fp);
        fwrite(&windHeading, sizeof(float), 1, fp);
        fwrite(&cumulusZ, sizeof(float), 1, fp);
        fwrite(&stratusZ, sizeof(float), 1, fp);
        fwrite(&contrailLow, sizeof(float), 1, fp);
        fwrite(&contrailHigh, sizeof(float), 1, fp);
    }
    // Cobra - compatibility with Tacedit
    /* Tacedit label              Cobra Variables
    ====================     =========================
      Wind Direction         (4) Wind Heading (degrees)
      Wind Speed             (4) Cumulus Base (feet)
      Time                   (4) Campaign/TE Time
      Temperature            (4) Stratus Base (feet)
      Temp                   (1) Temperature (deg. Celcius)
      Wind                   (1) Wind Speed (knots)
      Cloud base             (1) Weather condition (1=Sunny,2=fair,3=poor,4=inclement)
      Con Layer Start        (1) Contrail Base (100's of feet)
      Con Layer End          (1) Overcast Depth (100's of feet)
      X Off                  (4) Stratus 2 Base (future)
      Y Off                  (4) Cobra file version (do not change) */

    else
    {
        UINT uix = 0;
        BYTE uConv;
        char sConv;
        float fTemp;

        fwrite(&windHeading, sizeof(float), 1, fp);
        fTemp = -cumulusZ; // WindSpeed
        fwrite(&fTemp, sizeof(float), 1, fp);
        fwrite(&lastCheck, sizeof(CampaignTime), 1, fp);
        fTemp = -stratusZ; // Temperature
        fwrite(&fTemp, sizeof(float), 1, fp);
        sConv = (char)(int)temperature; // TodaysTemp
        fwrite(&sConv, sizeof(char), 1, fp);
        uConv = (BYTE)(windSpeed * (KPH_TO_FPS * FTPSEC_TO_KNOTS));
        fwrite(&uConv, sizeof(BYTE), 1, fp); // TodaysWind
        uConv = (BYTE)weatherCondition;
        fwrite(&uConv, sizeof(BYTE), 1, fp); // TodaysBase
        uConv = (BYTE)(int)(contrailLow / 1000.0f + 0.5); // same
        fwrite(&uConv, sizeof(BYTE), 1, fp);
        uConv = (BYTE)(int)(stratusDepth / 100.0f); // TodaysConHigh
        fwrite(&uConv, sizeof(BYTE), 1, fp);
        fTemp = -stratus2Z; // Temperature
        fwrite(&fTemp, sizeof(float), 1, fp); // offsetX
        fwrite(&COVersion, sizeof(float), 1, fp); // offsetY
        fwrite(&uix, sizeof(UINT), 1, fp); // map width
        fwrite(&uix, sizeof(UINT), 1, fp); // map height
        // fwrite(map,sizeof(CellState),w*h,fp);

        // Artscout - 2026 (FRONTS): the fronts, and for a campaign (which
        // reads nothing else from this file) the weather to resume with.
        // Readers that predate it stop at the map dimensions above.
        unsigned int ver = FRONTS_VERSION;
        unsigned int active = frontMap.active ? 1 : 0;
        fwrite(FRONTS_MAGIC, 4, 1, fp);
        fwrite(&ver, sizeof(ver), 1, fp);
        fwrite(&active, sizeof(active), 1, fp);
        fwrite(&frontMap.s, sizeof(frontMap.s), 1, fp);
        fwrite(&windHeading, sizeof(float), 1, fp);
        fwrite(&windSpeed, sizeof(float), 1, fp);
        fwrite(&temperature, sizeof(float), 1, fp);
    }

    CloseCampFile(fp);
    return 1;
}

float WeatherClass::TemperatureAt(const Tpoint *pos)
{
    float alt = -pos->z / 1000;
    float delta = 0.f;

    if (frontMap.active)
        frontMap.Extras(pos->x, pos->y, TheCampaign.CurrentTime, NULL, &delta);

    return temperature + delta - 3 * alt;
}

float WeatherClass::WindSpeedInFeetPerSecond(const Tpoint *pos)
{
    float boostKts = 0.f;

    // windSpeed is km/h; a front's boost is knots.
    if (frontMap.active and pos)
        frontMap.Extras(pos->x, pos->y, TheCampaign.CurrentTime, &boostKts,
                        NULL);

    return (windSpeed + boostKts * 1.852f) * 0.9113f;
}

float WeatherClass::WindHeadingAt(const Tpoint *pos)
{
    return windHeading;
}

// Artscout - 2026 (FRONTS): these were stubs returning 0 since the 2003
// rewrite, which is why no weather ever hid anything from a satellite.
// Grid x is east and y north; sim is the other way round.
int WeatherClass::GetCloudCover(GridIndex x, GridIndex y)
{
    return WeatherFrontMap::Cover(SeverityAt(GridToSim(y), GridToSim(x)));
}

// Hundreds of feet, as the old map kept it.
int WeatherClass::GetCloudLevel(GridIndex x, GridIndex y)
{
    return CloudBaseAtGrid(x, y) / 100;
}

int WeatherClass::ConditionAtGrid(GridIndex x, GridIndex y)
{
    return ConditionAt(GridToSim(y), GridToSim(x));
}

// The base of the cloud that matters there, in feet. Mirrors the shaping the
// renderer does in SampleLocal, so the briefing agrees with what you fly into.
int WeatherClass::CloudBaseAtGrid(GridIndex x, GridIndex y)
{
    if (not frontMap.active)
        return FloatToInt32(-stratusZ);

    float sev = SeverityAt(GridToSim(y), GridToSim(x));

    switch (WeatherFrontMap::Condition(sev))
    {
    case SUNNY:
        return 100 * 220;

    case FAIR:
        return max(FloatToInt32(-cumulusZ), 6000);

    case POOR:
        return FloatToInt32(15000.f - 7000.f * min(max(sev - 2.5f, 0.f), 1.f));

    default:
    {
        float wq = min(max(1.f - (sev - 3.5f), 0.05f), 1.f);
        return FloatToInt32(15000.f * wq + 5000.f - stratusDepth / 2.f);
    }
    }
}

// The local condition changed under the viewer: refit the layers to it the way
// Init does for a new day, without touching the prevailing condition.
void WeatherClass::OnLocalCondition(int condition)
{
    ApplyCondition(condition, false, false);

    cumulusZ = (float)-(100 * max(cumulusBase, 60) + 100 * (rand() % 5));

    if (weatherCondition > FAIR)
        stratusZ = (float)-(100 * stratusBase + 100 * (rand() % 20));
    else
        stratusZ = (float)-(100 * stratusBase + 100 * (rand() % 30));

    RealWeather::UpdateCondition();
}

void WeatherClass::ReadFronts(char *data, long size, int type)
{
    long need = FRONTS_OFFSET + 4 + (long)(2 * sizeof(unsigned int) +
                                           sizeof(WeatherFrontState) +
                                           3 * sizeof(float));

    if (not data or size < need or
        memcmp(data + FRONTS_OFFSET, FRONTS_MAGIC, 4) not_eq 0)
        return;

    char *p = data + FRONTS_OFFSET + 4;
    unsigned int ver = *(unsigned int *)p;
    p += sizeof(unsigned int);
    unsigned int active = *(unsigned int *)p;
    p += sizeof(unsigned int);

    if (ver not_eq FRONTS_VERSION or g_nWeatherFronts == 0 or
        type == game_Dogfight)
        return;

    memcpy(&frontMap.s, p, sizeof(WeatherFrontState));
    p += sizeof(WeatherFrontState);

    if (frontMap.s.count > FRONTS_MAX)
        frontMap.s.count = FRONTS_MAX;

    frontMap.active = active not_eq 0;

    char line[128];
    sprintf_s(line, sizeof(line),
              "FRONTS: file carries %u fronts, prevailing %.2f, active %u\n",
              frontMap.s.count, frontMap.s.prevailing, active);
    FFDebugLog(line);

    // A campaign reads nothing else from this file, so take the weather to
    // resume with from here. A TE has already read it from the fields above.
    if (type == game_Campaign or type == game_PlayerPool)
    {
        windHeading = *(float *)p;
        windSpeed = *(float *)(p + 4);
        temperature = *(float *)(p + 8);
    }

    int c = WeatherFrontMap::Condition(frontMap.s.prevailing);
    ApplyCondition(c, true, true);
}

void WeatherClass::SeedFronts(bool inProgress)
{
    float prevailing = (float)weatherCondition;
    frontMap.Clear(prevailing);
    frontMap.active = true;
    frontMap.s.noiseAmp = max(g_fWeatherNoise, 0.f);
    frontMap.s.noiseScale = 400000.f; // ~120 km patches
    frontMap.s.seed = (unsigned int)rand() * 32768u + (unsigned int)rand();

    float n = g_fWeatherFrontsPerDay * 0.75f;
    int count = (int)n + ((Rand01() < n - (int)n) ? 1 : 0);

    for (int i = 0; i < count; i++)
        SpawnFront(inProgress);

    EvolveFronts(0);

    char line[128];
    sprintf_s(line, sizeof(line),
              "FRONTS: seeded %u fronts, prevailing %.0f, noise %.2f\n",
              frontMap.s.count, prevailing, frontMap.s.noiseAmp);
    FFDebugLog(line);
}

// A new front. inProgress: somewhere in its life already and on the map, as
// at the start of a game; otherwise it comes in from the upwind edge (storm
// cells, which live only hours, form where they are).
bool WeatherClass::SpawnFront(bool inProgress)
{
    if (frontMap.s.count >= FRONTS_MAX)
        return false;

    const float KTS = 1.68781f; // knots to ft/s
    const float KM = GRID_SIZE_FT;

    float sizeX = (float)Map_Max_Y * KM; // sim x is north
    float sizeY = (float)Map_Max_X * KM;

    if (sizeX <= 0.f or sizeY <= 0.f)
        return false;

    float cx = sizeX * 0.5f, cy = sizeY * 0.5f;
    float radius = 0.5f * sqrtf(sizeX * sizeX + sizeY * sizeY);

    WeatherFront f;
    memset(&f, 0, sizeof(f));

    int kind;
    float r = Rand01();

    if (r < 0.35f)
        kind = FRONT_COLD;
    else if (r < 0.55f)
        kind = FRONT_WARM;
    else if (r < 0.72f)
        kind = FRONT_SQUALL;
    else if (r < 0.88f)
        kind = FRONT_CELL;
    else
        kind = FRONT_HIGH;

    // They travel with the upper wind, give or take.
    f.heading = windHeading + RandIn(-35.f, 35.f) * DTR;

    switch (kind)
    {
    case FRONT_COLD:
        f.halfWidth = RandIn(25.f, 45.f) * KM;
        f.halfLength = RandIn(150.f, 350.f) * KM;
        f.severity = RandIn(1.6f, 2.6f);
        f.windBoost = RandIn(10.f, 20.f);
        f.tempDelta = -RandIn(3.f, 7.f);
        f.speed = RandIn(20.f, 30.f) * KTS;
        break;

    case FRONT_WARM:
        f.halfWidth = RandIn(50.f, 80.f) * KM;
        f.halfLength = RandIn(200.f, 400.f) * KM;
        f.severity = RandIn(1.2f, 1.8f);
        f.windBoost = RandIn(5.f, 10.f);
        f.tempDelta = RandIn(2.f, 4.f);
        f.speed = RandIn(10.f, 18.f) * KTS;
        break;

    case FRONT_SQUALL:
        f.halfWidth = RandIn(8.f, 15.f) * KM;
        f.halfLength = RandIn(40.f, 100.f) * KM;
        f.severity = RandIn(2.2f, 3.0f);
        f.windBoost = RandIn(20.f, 35.f);
        f.tempDelta = -2.f;
        f.speed = RandIn(25.f, 40.f) * KTS;
        break;

    case FRONT_CELL:
        f.halfWidth = RandIn(8.f, 20.f) * KM;
        f.halfLength = 0.f;
        f.severity = RandIn(2.0f, 3.0f);
        f.windBoost = RandIn(15.f, 25.f);
        f.tempDelta = -2.f;
        f.speed = RandIn(15.f, 30.f) * KTS;
        break;

    default: // FRONT_HIGH
        f.halfWidth = RandIn(60.f, 120.f) * KM;
        f.halfLength = 0.f;
        f.severity = -RandIn(1.5f, 2.5f);
        f.windBoost = 0.f;
        f.tempDelta = 1.f;
        f.speed = RandIn(8.f, 15.f) * KTS;
        break;
    }

    float ch = cosf(f.heading), sh = sinf(f.heading);
    bool local = (kind == FRONT_CELL or kind == FRONT_SQUALL);
    float px, py; // where the core is now
    float lifeSecs;

    if (local)
    {
        px = RandIn(0.1f, 0.9f) * sizeX;
        py = RandIn(0.1f, 0.9f) * sizeY;
        lifeSecs = RandIn(3.f, 8.f) * 3600.f;
    }
    else
    {
        // Enter upwind, cross, leave downwind.
        float travel = 2.f * radius + 4.f * f.halfWidth;
        float lateral = RandIn(-0.6f, 0.6f) * radius;
        px = cx - ch * (radius + 2.f * f.halfWidth) - sh * lateral;
        py = cy - sh * (radius + 2.f * f.halfWidth) + ch * lateral;
        lifeSecs = travel / f.speed;
    }

    f.life = (unsigned int)(lifeSecs * 1000.f);

    unsigned int now = TheCampaign.CurrentTime;
    unsigned int age = 0;

    if (inProgress)
    {
        age = (unsigned int)(RandIn(0.2f, 0.6f) * (float)f.life);

        if (not local)
        {
            // Put it where it would have got to by now.
            px += ch * f.speed * (float)age * 0.001f;
            py += sh * f.speed * (float)age * 0.001f;
        }

        // Early in a campaign the clock is younger than the front would be.
        // Keep where it is and how long it has left; only its birth moves up.
        if (age > now)
        {
            f.life -= age - now;
            age = now;
        }
    }

    f.born = now - age;
    // Its origin is the core position at birth.
    f.x = px - ch * f.speed * (float)age * 0.001f;
    f.y = py - sh * f.speed * (float)age * 0.001f;

    static const char *names[] = {"cold front", "warm front", "squall line",
                                  "storm cell", "clearing"};
    char line[192];
    sprintf_s(line, sizeof(line),
              "FRONTS: %s %s, core %.0f,%.0f km (E,N), heading %.0f, "
              "%.0f kts, severity %+.1f, %.1f of %.1f h\n",
              inProgress ? "in progress:" : "new:", names[kind],
              py / KM, px / KM, fmodf(f.heading / DTR + 720.f, 360.f),
              f.speed / KTS, f.severity,
              age / 3600000.f, f.life / 3600000.f);
    FFDebugLog(line);

    return frontMap.Add(f);
}

// Master only: age the fronts, keep the random patches riding the wind, and
// now and then bring a new front in.
void WeatherClass::EvolveFronts(CampaignTime dt)
{
    frontMap.Expire(TheCampaign.CurrentTime);

    float fps = windSpeed * 0.9113f;
    frontMap.s.driftX = cosf(windHeading) * fps;
    frontMap.s.driftY = sinf(windHeading) * fps;

    if (dt <= 0 or g_fWeatherFrontsPerDay <= 0.f)
        return;

    // At most six hours at once, so a long gap does not flood the map.
    float hours = min((float)dt / (float)CampaignHours, 6.f);
    float chance = g_fWeatherFrontsPerDay * hours / 24.f;

    while (chance > 0.f)
    {
        if (Rand01() < min(chance, 1.f))
            SpawnFront(false);

        chance -= 1.f;
    }
}

//FIXME
void WeatherClass::SetCloudCover(GridIndex x, GridIndex y, int cov)
{
}

//FIXME
void WeatherClass::SetCloudLevel(GridIndex x, GridIndex y, int lev)
{
}
