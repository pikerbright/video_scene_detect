# coding:utf-8
import os
import json
import argparse


def transcoding(filename):
    cmd = 'ffmpeg -y -i ' + filename + ' -vcodec libx264 -an -g 1000 -profile:v baseline -sc_threshold 125 temp.mp4'
    os.system(cmd)


def get_video_info():
    cmd = 'ffprobe -show_frames -print_format json save.mp4'
    json_obj = json.load(os.popen(cmd))

    return json_obj


def parse_args():
    parser = argparse.ArgumentParser(description='process scene change',
                                     formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('input_file', help='input video file name', type=str)
    parser.add_argument('output_file', help='output .srt file name ', type=str)

    args = parser.parse_args()
    return args


def write_scene_file(filename):
    pts_times = []
    for frame_info in info['frames']:
        if frame_info['media_type'] == 'video' and frame_info['pict_type'] == 'I':
            pts_times.append(float(frame_info['pkt_pts_time']))

    detect_result = []
    with open(filename, 'w') as f:
        for i in range(len(pts_times) - 1):
            start_pts_time = pts_times[i]
            end_pts_time = pts_times[i + 1]
            f.write("{}\n".format(i + 1))
            s_hour = int(start_pts_time / 3600.0)
            s_minute = int((start_pts_time - s_hour * 3600) / 60.0)
            s_second = start_pts_time - s_hour * 3600 - s_minute * 60

            e_hour = int(end_pts_time / 3600.0)
            e_minute = int((end_pts_time - s_hour * 3600) / 60.0)
            e_second = end_pts_time - e_hour * 3600 - e_minute * 60

            f.write("{:0>2}:{:0>2}:{:.3f} --> {:0>2}:{:0>2}:{:.3f}\n".format(s_hour, s_minute, s_second, e_hour,
                                                                             e_minute, e_second))
            f.write("镜头{}\n\n".format(i + 1))
            str_start_time = "{:0>2}:{:0>2}:{:.3f}".format(s_hour, s_minute, s_second)
            str_end_time = "{:0>2}:{:0>2}:{:.3f}".format(e_hour, e_minute, e_second)
            detect_result.append({'index': i + 1, 'start_time': str_start_time, 'end_time': str_end_time})

        f.write("{}\n".format(i + 1))
        f.write("{:0>2}:{:0>2}:{:.3f} --> {:0>2}:{:0>2}:{:.3f}\n".format(e_hour, e_minute, e_second, 10, 0, 0))
        f.write("镜头{}\n\n".format(i + 1))

        str_start_time = "{:0>2}:{:0>2}:{:.3f}".format(e_hour, e_minute, e_second)
        str_end_time = "{:0>2}:{:0>2}:{:.3f}".format(10, 0, 0)
        detect_result.append({'index': i + 1, 'start_time': str_start_time, 'end_time': str_end_time})

    json_file_name = filename.split('.')[0] + '.json'
    with open(json_file_name, 'w') as f:
        json.dump(detect_result, f)


if __name__ == "__main__":
    args = parse_args()

    transcoding(args.input_file)
    info = get_video_info()
    write_scene_file(args.output_file)

