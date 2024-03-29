
#include <unistd.h>

#include <cctype>
#include <regex>
#include <unordered_set>

#include "base/map.h"
#include "feature/feature_processing.h"
#include "geometry/epipolar_geometry.hpp"
#include "geometry/track_processor.h"
#include "optimization/ba_solver.h"
#include "utility/io_ecim.hpp"
#include "utility/timer.h"
// #include "utility/view.h"
// #include "utility/viewer.h"

using namespace xrsfm;

void PreProcess(const std::string bin_path, const std::string feature_path,
                const std::string frame_pair_path, Map &map) {
    std::vector<Frame> frames;
    std::vector<FramePair> frame_pairs;

    std::map<int, Frame> frames_pose, frames_pt;
    ReadImagesBinary(bin_path + "images.bin", frames_pose);
    ReadImagesBinaryForTriangulation(feature_path, frames_pt);
    printf("frame number(reconstruction): %zu frame number(matching): %zu\n",
           frames_pose.size(), frames_pt.size());

    std::map<std::string, int> name2id_recon;
    std::map<std::string, int> name2id_feature;

    // set frames(TODO change vector to map)
    int max_id = -1;
    for (const auto &[id, frame] : frames_pt) {
        CHECK(frame.id == id);
        CHECK(frames_pose.count(id) != 0) << "Missing corresponding frame id";
        CHECK(frames_pose.at(id).name == frame.name)
            << "inconsistent frame name";
        max_id = std::max(max_id, id);
    }
    frames.resize(max_id + 1);

    for (int id = 0; id < frames.size(); ++id) {
        auto &frame = frames[id];
        frame.id = id;
        frame.registered = false;
        if (frames_pt.count(id) != 0) {
            frame = frames_pt.at(id);
            frame.camera_id = frames_pose.at(id).camera_id;
            frame.Tcw = frames_pose.at(id).Tcw;
            frame.registered = true;
        }
    }

    // set cameras & image name
    std::map<int, Camera> cameras;
    ReadCamerasBinary(bin_path + "cameras.bin", cameras);

    // set points for reconstruction
    for (auto &frame : frames) {
        const int num_points = frame.keypoints_.size();
        frame.points.clear();
        frame.track_ids_.assign(num_points, -1);
        for (const auto &kpt : frame.keypoints_) {
            const auto &pt = kpt.pt;
            frame.points.emplace_back(Eigen::Vector2d(pt.x, pt.y));
        }
    }

    ReadFramePairBinaryForTriangulation(frame_pair_path, frame_pairs);
    std::vector<FramePair> filtered_frame_pairs;
    const int num_fp = frame_pairs.size();
    int count = 0;
    for (auto &frame_pair : frame_pairs) {
        const int num_matches = frame_pair.matches.size();
        if (num_matches < 30)
            continue;
        count++;
        if (count % 100 == 0)
            std::cout << 1.0 * count / num_fp << std::endl;
        // std::cout<<frame_pair.id1<<" "<<frame_pair.id2<<std::endl;
        auto &frame1 = frames[frame_pair.id1];
        auto &frame2 = frames[frame_pair.id2];
        std::vector<Eigen::Vector2d> points1, points2;
        for (const auto &match : frame_pair.matches) {
            points1.push_back(frame1.points[match.id1]);
            points2.push_back(frame2.points[match.id2]);
        }
        SolveFundamnetalCOLMAP(points1, points2, frame_pair);
        // std::cout<<frame_pair.inlier_num<<" "<<num_matches<<std::endl;
        if (frame_pair.inlier_num < 30)
            continue;
        std::vector<Match> new_matches;
        for (int i = 0; i < num_matches; ++i) {
            if (frame_pair.inlier_mask[i])
                new_matches.push_back(frame_pair.matches[i]);
        }
        frame_pair.matches = new_matches;
        frame_pair.inlier_mask.assign(frame_pair.matches.size(), true);
        filtered_frame_pairs.push_back(frame_pair);
    }
    frame_pairs = filtered_frame_pairs;

    map.frames_ = frames;
    map.camera_map_ = cameras;
    map.frame_pairs_ = frame_pairs;
    map.Init();
}

int main(int argc, const char *argv[]) {
    google::InitGoogleLogging(argv[0]);
    // 1.Read Config
    std::string bin_path, feature_path, matches_path, output_path;
    if (argc <= 2) {
        std::string config_path = "./config_tri.json";
        if (argc == 2) {
            config_path = argv[1];
        }
        auto config_json = LoadJSON(config_path);
        bin_path = config_json["bin_path"];
        feature_path = config_json["feature_path"];
        matches_path = config_json["matches_path"];
        output_path = config_json["output_path"];
    } else if (argc == 5) {
        bin_path = argv[1];
        feature_path = argv[2];
        matches_path = argv[3];
        output_path = argv[4];
    } else {
        exit(-1);
    }

    Map map;
    PreProcess(bin_path, feature_path, matches_path, map);

    BASolver ba_solver;
    Point3dProcessor p3d_processor;
    // TriangulateImage
    for (auto &frame : map.frames_) {
        printf("current frame: %d\n", frame.id);
        if (frame.registered) {
            p3d_processor.TriangulateFramePoint(map, frame.id, 8.0);
        } else {
            std::cout << frame.id << " not registered\n";
        }
    }
    std::cout << "Triangulate Done\n";

    // Complete Tracks
    const int num_track = map.tracks_.size();
    for (int track_id = 0; track_id < num_track; ++track_id) {
        if (map.tracks_[track_id].outlier)
            continue;
        p3d_processor.ContinueTrack(map, track_id, 8.0);
    }

    // Merge Tracks
    for (int track_id = 0; track_id < num_track; ++track_id) {
        if (map.tracks_[track_id].outlier)
            continue;
        p3d_processor.MergeTrack(map, track_id, 8.0);
    }

    // Remove Frames
    for (auto &frame : map.frames_) {
        int num_mea = 0;
        for (int i = 0; i < frame.track_ids_.size(); ++i) {
            if (frame.track_ids_[i] == -1)
                continue;
            num_mea++;
        }
        if (num_mea == 0) {
            frame.registered = false;
        }
    }

    // GBA
    ba_solver.GBA(map, true, true);

    WriteColMapDataBinary(output_path, map);
    return 0;
}
