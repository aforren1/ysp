/* dataset_golden.h - the golden bytes of dataset_test.c's synthetic session.
 * Made by `dataset_test --golden` and reviewed by hand; a change here is a
 * change of the data set format or of the writer, so say which in the
 * commit. A CSV field with an LF inside is one row (events.csv). */

#define GOLD_SESSION_START \
    "{\n" \
    "  \"closed\": false,\n" \
    "  \"devices\": [\n" \
    "    {\n" \
    "      \"family\": \"keyboard\",\n" \
    "      \"role\": \"resp\"\n" \
    "    }\n" \
    "  ],\n" \
    "  \"experiment\": \"sha256:00ff\",\n" \
    "  \"fields\": {\n" \
    "    \"participant\": 7\n" \
    "  },\n" \
    "  \"format\": \"ysp-session\",\n" \
    "  \"onset_offset_s\": 0.0015,\n" \
    "  \"refresh_hz\": 59.94,\n" \
    "  \"seed\": 20261021,\n" \
    "  \"start_utc\": \"2026-10-21T12:00:00Z\",\n" \
    "  \"version\": 1\n" \
    "}\n"

#define GOLD_SESSION_END \
    "{\n" \
    "  \"changes\": [\n" \
    "    {\n" \
    "      \"at_trial\": 2,\n" \
    "      \"experiment\": \"sha256:0100\"\n" \
    "    }\n" \
    "  ],\n" \
    "  \"closed\": true,\n" \
    "  \"devices\": [\n" \
    "    {\n" \
    "      \"family\": \"keyboard\",\n" \
    "      \"role\": \"resp\"\n" \
    "    }\n" \
    "  ],\n" \
    "  \"experiment\": \"sha256:00ff\",\n" \
    "  \"fields\": {\n" \
    "    \"participant\": 7\n" \
    "  },\n" \
    "  \"format\": \"ysp-session\",\n" \
    "  \"onset_offset_s\": 0.0015,\n" \
    "  \"refresh_hz\": 59.94,\n" \
    "  \"rows\": {\n" \
    "    \"clock\": 2,\n" \
    "    \"events\": 5,\n" \
    "    \"frames\": 4,\n" \
    "    \"streams\": {\n" \
    "      \"dots\": 3\n" \
    "    },\n" \
    "    \"trials\": 3\n" \
    "  },\n" \
    "  \"seed\": 20261021,\n" \
    "  \"self_test\": {\n" \
    "    \"fullscreen\": true\n" \
    "  },\n" \
    "  \"start_utc\": \"2026-10-21T12:00:00Z\",\n" \
    "  \"version\": 1\n" \
    "}\n"

#define GOLD_TRIALS \
    "participant,trial_index,response,correct,rt_s,level,note\n" \
    "P07,0,left,1,0.412345678,0.05,\n" \
    "P07,1,,0,,0.1,no response\n" \
    "P07,2,\"right, \"\"quoted\"\"\",1,0.412347678,0.1,\n"

#define GOLD_FRAMES \
    "frame,trial_index,target_s,planned_s,onset_s,residual_s,dropped,path,tier,flags,code_risk,evaluate_s,script_s,draw_s,upload_s,swap_s,gpu_s\n" \
    "100,0,5.000000000,5.000000120,5.000035120,0.000035120,0,1,2,0,0,0.000012000,0.000000000,0.000250000,0.000000000,0.000900000,\n" \
    "101,0,5.016666667,5.016666787,5.016701787,0.000035120,0,1,2,0,0,0.000012000,0.000000000,0.000250001,0.000000000,0.000900000,\n" \
    "102,1,5.033333334,5.033333454,5.033368454,0.000035120,1,1,3,4,0,0.000012000,0.000000000,0.000250002,0.000000000,0.000900000,\n" \
    "103,1,5.050000001,5.050000121,,,0,1,2,0,0,0.000012000,0.000000000,0.000250003,0.000000000,0.000900000,\n"

#define GOLD_EVENTS \
    "t_s,trial_index,kind,role,stimulus,frame,planned_s,residual_s,code,control,type,value,x,y,deadline_s,end_s,tier,flags,ticks,text\n" \
    "5.016701787,0,onset,,gabor,101,5.016666787,0.000035000,0,2,,,,,,,,1,,\n" \
    "5.420000000,0,keyboard,resp,,,,,97,4,1,1,0,0,,,2,16,123456,\n" \
    "5.420000000,0,response,resp,,,,,,,,,,,,,,,,left\n" \
    "5.016670000,0,out,trig,,,,0.000003213,12,,,,,,5.016666787,5.016690000,,0,,\n" \
    "5.500000000,,visibility,,,,,,,,,,,,,,,,,\"hidden, then \"\"visible\"\"\n" \
    "again\"\n"

#define GOLD_CLOCK \
    "role,kind,t_s,ticks,width_s,ns_per_tick,ppm,spread_s,span_s,points,epoch\n" \
    "photo,pair,4.000000000,1000,0.000041000,,,,,,\n" \
    "photo,fit,4.000000000,1000,0.000041000,1000.0002,0.2,0.000015000,5.000000000,25,0\n"

#define GOLD_DOTS \
    "t_s,seed,updates\n" \
    "5.016701787,77,0\n" \
    "5.033368454,77,1\n" \
    "5.050035121,77,2\n"

/* FNV-1a 64 of the zip of the finished folder (yds_zip()), and its size. */
#define GOLD_ZIP_FNV 0xff0f979e7fc25d27ULL
#define GOLD_ZIP_SIZE 2883
