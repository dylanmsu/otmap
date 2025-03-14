// This file is part of otmap, an optimal transport solver.
//
// Copyright (C) 2017-2018 Gael Guennebaud <gael.guennebaud@inria.fr>
// Copyright (C) 2017 Georges Nader
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <iostream>
#include "common/otsolver_options.h"
#include "utils/eigen_addons.h"
#include "common/image_utils.h"
#include "common/generic_tasks.h"
#include "utils/BenchTimer.h"
#include <surface_mesh/Surface_mesh.h>
#include "utils/rasterizer.h"


#include "normal_integration/normal_integration.h"
#include "normal_integration/mesh.h"

using namespace Eigen;
using namespace surface_mesh;
using namespace otmap;

void output_usage()
{
  std::cout << "usage : sample <option> <value>" << std::endl;

  std::cout << std::endl;

  std::cout << "input options : " << std::endl;
  std::cout << " * -in <filename> -> input image" << std::endl;

  std::cout << std::endl;

  CLI_OTSolverOptions::print_help();

  std::cout << std::endl;

  std::cout << " * -ores <res1> <res2> <res3> ... -> ouput point resolutions" << std::endl;
  std::cout << " * -ptscale <value>               -> scaling factor to apply to SVG point sizes (default 1)" << std::endl;
  std::cout << " * -pattern <value>               -> pattern = poisson or a .dat file, default is tiling from uniform_pattern_sig2012.dat" << std::endl;
  std::cout << " * -export_maps                   -> write maps as .off files" << std::endl;

  std::cout << std::endl;

  std::cout << "output options :" << std::endl;
  std::cout << " * -out <prefix>" << std::endl;
}

struct CLIopts : CLI_OTSolverOptions
{
  std::string filename_src;
  std::string filename_trg;

  VectorXi ores;
  double pt_scale;
  std::string pattern;
  bool inv_mode;
  bool export_maps;

  uint resolution;

  std::string out_prefix;

  double focal_l;

  void set_default()
  {
    filename_src = "";
    filename_trg = "";

    ores.resize(1); ores.setZero();
    ores(0) = 1;

    out_prefix = "";

    pt_scale = 1;
    export_maps = 0;
    pattern = "";

    resolution = 100;

    focal_l = 1.0;

    CLI_OTSolverOptions::set_default();
  }

  bool load(const InputParser &args)
  {
    set_default();

    CLI_OTSolverOptions::load(args);

    std::vector<std::string> value;

    if(args.getCmdOption("-in_src", value))
      filename_src = value[0];
    else
      return false;

    if(args.getCmdOption("-in_trg", value))
      filename_trg = value[0];
    else
      return false;

    /*if(args.getCmdOption("-points", value))
      pattern = value[0];
    else
      return false;*/

    if(args.getCmdOption("-res", value))
      resolution = std::atof(value[0].c_str());

    if(args.getCmdOption("-ptscale", value))
      pt_scale = std::atof(value[0].c_str());
    
    if(args.cmdOptionExists("-export_maps"))
      export_maps = true;

    if(args.getCmdOption("-focal_l", value))
      focal_l = std::atof(value[0].c_str());

    return true;
  }
};

template<typename T,typename S>
T lerp(S u, const T& a0, const T& a1)
{
  return (1.-u)*a0 + u*a1;
}

void interpolate(const std::vector<Surface_mesh> &inv_maps, double alpha, Surface_mesh& result)
{
  //clear output
  result.clear();
  result = inv_maps[0];

  int nv = result.vertices_size();

  for(int j=0; j<nv; ++j){
    Surface_mesh::Vertex v(j);
    // linear interpolation
    result.position(v) = lerp(alpha,inv_maps[0].position(v),inv_maps[1].position(v));
  }
}

void synthetize_and_save_image(const Surface_mesh& map, const std::string& filename, int res, double expected_mean, bool inv)
{
  MatrixXd img(res,res);
  rasterize_image(map, img);
  img = img * (expected_mean/img.mean());

  if(inv)
    img = 1.-img.array();

  save_image(filename.c_str(), img);
}

std::vector<double> normalize_vec(std::vector<double> p1) {
    std::vector<double> vec(3);
    double squared_len = 0;
    for (int i=0; i<p1.size(); i++) {
        squared_len += p1[i] * p1[i];
    }

    double len = std::sqrt(squared_len);

    for (int i=0; i<p1.size(); i++) {
        vec[i] = p1[i] / len;
    }

    return vec;
}


// Function to calculate the gradient of f(y, z)
void gradient(  std::vector<double> source,
                std::vector<double> interf,
                std::vector<double> target,
                double n1, double n2,
                double & grad_x, double & grad_y) {
    double d1 = std::sqrt((interf[0] - source[0]) * (interf[0] - source[0]) + (interf[1] - source[1]) * (interf[1] - source[1]) + (interf[2] - source[2]) * (interf[2] - source[2]));
    double d2 = std::sqrt((target[0] - interf[0]) * (target[0] - interf[0]) + (target[1] - interf[1]) * (target[1] - interf[1]) + (target[2] - interf[2]) * (target[2] - interf[2]));

    grad_x = n1 * (interf[0] - source[0]) / d1 - n2 * (target[0] - interf[0]) / d2;
    grad_y = n1 * (interf[1] - source[1]) / d1 - n2 * (target[1] - interf[1]) / d2;
}

void scaleAndTranslatePoints(std::vector<std::vector<double>>& points, double MAX_X, double MAX_Y, double margin) {
    double scaleFactorX = (MAX_X - 2 * margin) / MAX_X;
    double scaleFactorY = (MAX_Y - 2 * margin) / MAX_Y;

    for (auto& point : points) {
        double& x = point[0];
        double& y = point[1];

        x = x * scaleFactorX;
        y = y * scaleFactorY;

        x += margin;
        y += margin;
    }
}

void export_grid_to_svg(std::vector<std::vector<double>> &points, double width, double height, int res_x, int res_y, std::string filename, double stroke_width) {
    std::ofstream svg_file(filename, std::ios::out);
    if (!svg_file.is_open()) {
        std::cerr << "Error: Unable to open file " << filename << std::endl;
    }

    // Write SVG header
    svg_file << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n";
    svg_file << "<svg width=\"1000\" height=\"" << 1000.0f * (height / width) << "\" xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">\n";

    svg_file << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";

    for (int j = 0; j < res_y; j++) {
        std::string path_str = "M";
        for (int i = 0; i < res_x; i++) {
            int idx = i + j * res_x;

            const auto& point = points[idx];
            path_str += std::to_string((point[0] / width) * 1000.0f) + "," +
                        std::to_string((point[1] / height) * 1000.0f * (height / width));
            if (i < res_x - 1)
                path_str += "L";
        }
        svg_file << "<path d=\"" << path_str << "\" fill=\"none\" stroke=\"black\" stroke-width=\"" << stroke_width << "\"/>\n";
    }

    for (int j = 0; j < res_x; j++) {
        std::string path_str = "M";
        for (int i = 0; i < res_y; i++) {
            int idx = j + i * res_x;

            const auto& point = points[idx];
            path_str += std::to_string((point[0] / width) * 1000.0f) + "," +
                        std::to_string((point[1] / height) * 1000.0f * (height / width));

            if (i < res_x - 1)
                path_str += "L";
        }
        svg_file << "<path d=\"" << path_str << "\" fill=\"none\" stroke=\"black\" stroke-width=\"" << stroke_width << "\"/>\n";
    }

    // Write SVG footer
    svg_file << "</svg>\n";
    svg_file.close();
}

void export_triangles_to_svg(std::vector<std::vector<double>> &points, std::vector<std::vector<unsigned int>> &triangles, double width, double height, int res_x, int res_y, std::string filename, double stroke_width) {
    std::ofstream svg_file(filename, std::ios::out);
    if (!svg_file.is_open()) {
        std::cerr << "Error: Unable to open file " << filename << std::endl;
    }

    // Write SVG header
    svg_file << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n";
    svg_file << "<svg width=\"1000\" height=\"" << 1000.0f * (height / width) << "\" xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">\n";
    
    svg_file << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";

    // Draw polygons
    for (const auto& polygon : triangles) {
        std::vector<std::vector<double>> poly_points;
        for (const auto& point_idx : polygon) {
            poly_points.push_back(points[point_idx]);
        }

        std::string path_str = "M";
        for (std::size_t j = 0; j < poly_points.size(); ++j) {
            const auto& point = poly_points[j];
            path_str += std::to_string((point[0] / width) * 1000.0f) + "," +
                        std::to_string((point[1] / height) * 1000.0f * (height / width));

            if (j < poly_points.size() - 1)
                path_str += "L";
        }
        path_str += "Z";
        svg_file << "<path d=\"" << path_str << "\" fill=\"none\" stroke=\"black\" stroke-width=\"" << stroke_width << "\"/>\n";
    }

    // Write SVG footer
    svg_file << "</svg>\n";
    svg_file.close();
}


void scalePoints(std::vector<std::vector<double>>& trg_pts, const std::vector<double>& scale, const std::vector<double>& origin) {
    for (std::vector<double>& point : trg_pts) {
        for (size_t j = 0; j < point.size(); ++j) {
            // Scale each dimension relative to the origin
            point[j] = origin[j] + (point[j] - origin[j]) * scale[j];
        }
    }
}

void translatePoints(std::vector<std::vector<double>>& trg_pts, std::vector<double> position_xyz) {
  for (int i = 0; i < trg_pts.size(); i++)
  {
    trg_pts[i][0] += position_xyz[0];
    trg_pts[i][1] += position_xyz[1];
    trg_pts[i][2] += position_xyz[2];
  }
}

// Define the rotation function
void rotatePoints(std::vector<std::vector<double>>& trg_pts, std::vector<double> angle_xyz) {
    double PI = 3.14159265358979323846;

    // Convert angles from degrees to radians
    angle_xyz[0] = angle_xyz[0] * PI / 180.0;
    angle_xyz[1] = angle_xyz[1] * PI / 180.0;
    angle_xyz[2] = angle_xyz[2] * PI / 180.0;

    // Precompute sine and cosine values for each rotation angle
    double cos_x = std::cos(angle_xyz[0]);
    double sin_x = std::sin(angle_xyz[0]);
    double cos_y = std::cos(angle_xyz[1]);
    double sin_y = std::sin(angle_xyz[1]);
    double cos_z = std::cos(angle_xyz[2]);
    double sin_z = std::sin(angle_xyz[2]);

    // Define the rotation matrices for each axis
    std::vector<std::vector<double>> rot_x = {
        {1, 0, 0},
        {0, cos_x, -sin_x},
        {0, sin_x, cos_x}
    };

    std::vector<std::vector<double>> rot_y = {
        {cos_y, 0, sin_y},
        {0, 1, 0},
        {-sin_y, 0, cos_y}
    };

    std::vector<std::vector<double>> rot_z = {
        {cos_z, -sin_z, 0},
        {sin_z, cos_z, 0},
        {0, 0, 1}
    };

    // Apply rotation to each point in the point cloud
    for (std::vector<double>& point : trg_pts) {
        // Extract x, y, z for clarity
        double x = point[0];
        double y = point[1];
        double z = point[2];

        // Rotate around x-axis
        double new_y = rot_x[1][1] * y + rot_x[1][2] * z;
        double new_z = rot_x[2][1] * y + rot_x[2][2] * z;
        y = new_y;
        z = new_z;

        // Rotate around y-axis
        double new_x = rot_y[0][0] * x + rot_y[0][2] * z;
        new_z = rot_y[2][0] * x + rot_y[2][2] * z;
        x = new_x;
        z = new_z;

        // Rotate around z-axis
        new_x = rot_z[0][0] * x + rot_z[0][1] * y;
        new_y = rot_z[1][0] * x + rot_z[1][1] * y;
        x = new_x;
        y = new_y;

        // Update the point with the rotated coordinates
        point[0] = x;
        point[1] = y;
        point[2] = z;
    }
}

TransportMap runOptimalTransport(MatrixXd &density, CLIopts &opts) {
  GridBasedTransportSolver otsolver;
  otsolver.set_verbose_level(opts.verbose_level-1);

  if(opts.verbose_level>=1)
    std::cout << "Generate transport map...\n";

  if(density.maxCoeff()>1.)
    density = density / density.maxCoeff(); //normalize

  BenchTimer t_solver_init, t_solver_compute, t_generate_uniform;

  t_solver_init.start();
  otsolver.init(density.rows());
  t_solver_init.stop();

  std::cout << "init\n";

  t_solver_compute.start();
  TransportMap tmap_src = otsolver.solve(vec(density), opts.solver_opt);
  t_solver_compute.stop();

  std::cout << "STATS solver -- init: " << t_solver_init.value(REAL_TIMER) << "s  solve: " << t_solver_compute.value(REAL_TIMER) << "s\n";

  return tmap_src;
}

void applyTransportMapping(TransportMap &tmap_src, TransportMap &tmap_trg, MatrixXd &density_trg, std::vector<Eigen::Vector2d> &vertex_positions) {
  Surface_mesh map_uv = tmap_src.fwd_mesh();
  Surface_mesh map_orig = tmap_src.origin_mesh();

  apply_inverse_map(tmap_trg, map_uv.points(), 3);

  auto originMeshPtr = std::make_shared<surface_mesh::Surface_mesh>(map_uv);
  auto fwdMeshPtr = std::make_shared<surface_mesh::Surface_mesh>(map_orig);
  auto densityPtr = std::make_shared<Eigen::VectorXd>(density_trg);

  TransportMap transport(originMeshPtr, fwdMeshPtr, densityPtr);
  
  apply_inverse_map(transport, vertex_positions, 3);
}

std::vector<double> cross(std::vector<double> v1, std::vector<double> v2){
  std::vector<double> result(3);
  result[0] = v1[1]*v2[2] - v1[2]*v2[1];
  result[1] = v1[2]*v2[0] - v1[0]*v2[2];
  result[2] = v1[0]*v2[1] - v1[1]*v2[0];
  return result;
}

double dot(std::vector<double> a, std::vector<double> b) {
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

std::vector<double> mult(double a, std::vector<double> b) {
  return {a*b[0], a*b[1], a*b[2]};
}

std::vector<double> add(std::vector<double> a, std::vector<double> b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

std::vector<double> sub(std::vector<double> a, std::vector<double> b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

double magnitude(std::vector<double> a) {
  return std::sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
}

// https://stackoverflow.com/questions/29758545/how-to-find-refraction-vector-from-incoming-vector-and-surface-normal
/*std::vector<double> refract(const std::vector<double>& normal, const std::vector<double>& incident, double n1, double n2) {
    // Ratio of refractive indices
    const double n = n1 / n2;
    // Calculate cos(theta_i), assuming normal and incident are unit vectors
    const double cosI = -dot(normal, incident);
    // Calculate sin^2(theta_t) using Snell's law
    const double sinT2 = n * n * (1.0 - cosI * cosI);
    
    // Check for Total Internal Reflection (TIR)
    if (sinT2 > 1.0) {
        // TIR occurs; return an invalid vector or handle appropriately
        // return Vector::invalid; // Uncomment if you have a way to represent TIR
    }
    
    // Calculate cos(theta_t)
    const double cosT = sqrt(1.0 - sinT2);
    // Calculate the refracted direction vector
    return add(mult(n, incident), mult((n * cosI - cosT), normal));
}*/

std::vector<double> refract(
    const std::vector<double>& surfaceNormal,
    const std::vector<double>& rayDirection,
    double n1,  // Index of refraction of the initial medium
    double n2   // Index of refraction of the second medium
) {
    // Check that both vectors have three components
    if (surfaceNormal.size() != 3 || rayDirection.size() != 3) {
        throw std::invalid_argument("Vectors must have exactly three components.");
    }

    // Calculate the ratio of indices of refraction
    double nRatio = n1 / n2;

    // Calculate the dot product of surfaceNormal and rayDirection
    double dotProduct = surfaceNormal[0] * rayDirection[0] +
                        surfaceNormal[1] * rayDirection[1] +
                        surfaceNormal[2] * rayDirection[2];

    // Determine the cosine of the incident angle
    double cosThetaI = -dotProduct;  // Cosine of the angle between the ray and the normal

    // Calculate sin^2(thetaT) using Snell's Law
    double sin2ThetaT = nRatio * nRatio * (1.0 - cosThetaI * cosThetaI);

    // Compute cos(thetaT) for the refracted angle
    double cosThetaT = std::sqrt(1.0 - sin2ThetaT);

    // Calculate the refracted ray direction
    std::vector<double> refractedRay(3);
    for (int i = 0; i < 3; ++i) {
        refractedRay[i] = nRatio * rayDirection[i] + 
                          (nRatio * cosThetaI - cosThetaT) * surfaceNormal[i];
    }

    return refractedRay;
}

// https://www.scratchapixel.com/lessons/3d-basic-rendering/minimal-ray-tracer-rendering-simple-shapes/ray-plane-and-ray-disk-intersection.html
bool intersect_plane(const std::vector<double> &n, const std::vector<double> &p0, const std::vector<double> &l0, const std::vector<double> &l, std::vector<double> &intersectionPoint) {
    double denom = dot(n, l);
    if (denom > 1e-6) { // Check if ray is not parallel to the plane
        std::vector<double> p0l0 = sub(p0, l0);
        double t = dot(p0l0, n) / denom; 
        if (t >= 0) { // Check if intersection is in the positive direction of the ray
            intersectionPoint[0] = l0[0] + t * l[0];
            intersectionPoint[1] = l0[1] + t * l[1];
            intersectionPoint[2] = l0[2] + t * l[2];
            return true;
        }
    }
    return false;
}

std::vector<double> calc_plane_normal(const std::vector<double> &A, const std::vector<double> &B, const std::vector<double> &C) {
    std::vector<double> edge1 = sub(B, A);
    std::vector<double> edge2 = sub(C, A);
    std::vector<double> normal = cross(edge1, edge2);
    return normalize_vec(normal); // Normalize the result to get a unit normal
}

bool is_boundary_vertex(Mesh &mesh, std::vector<std::pair<int, int>> &adjacent_edges, std::vector<int> &adjacent_triangles, int vertex_index, std::vector<std::pair<int, int>>& boundary_edges) {
    std::unordered_map<std::pair<int, int>, int, HashPair> edge_triangle_count;
    for (int triangle_index : adjacent_triangles) {
        const std::vector<unsigned int>& triangle = mesh.triangles[triangle_index];
        for (int j = 0; j < 3; ++j) {
            int v1 = triangle[j];
            int v2 = triangle[(j + 1) % 3];
            std::pair<int, int> edge = std::make_pair(std::min(v1, v2), std::max(v1, v2));
            edge_triangle_count[edge]++;
        }
    }

    bool is_boundary = false;
    for (const auto& edge : adjacent_edges) {
        if (edge_triangle_count[edge] == 1) { // Boundary edge
            boundary_edges.push_back(edge);
            is_boundary = true;
        }
    }

    return is_boundary;
}

void project_onto_boundary(std::vector<double> &point) {
  point[0] -= 0.5;
  point[1] -= 0.5;

  double dist = sqrt(pow(point[0], 2) + pow(point[1], 2))*2;

  point[0] /= dist;
  point[1] /= dist;

  point[0] += 0.5;
  point[1] += 0.5;
}

//compute the desired normals
std::vector<std::vector<double>> fresnelMapping(
  std::vector<std::vector<double>> &vertices, 
  std::vector<std::vector<double>> &target_pts, 
  double refractive_index
) {
    std::vector<std::vector<double>> desiredNormals;

    //double boundary_z = -0.1;

    //vector<std::vector<double>> boundary_points;

    bool use_point_src = false;
    bool use_reflective_caustics = false;

    std::vector<double> pointLightPosition(3);
    pointLightPosition[0] = 0.5;
    pointLightPosition[1] = 0.5;
    pointLightPosition[2] = 0.5;

    // place initial points on the refractive surface where the light rays enter the material
    /*if (use_point_src && !use_reflective_caustics) {
        for(int i = 0; i < vertices.size(); i++) {
            std::vector<double> boundary_point(3);

            // ray to plane intersection to get the initial points
            double t = ((boundary_z - pointLightPosition[2]) / (vertices[i][2] - pointLightPosition[2]));
            boundary_point[0] = pointLightPosition[0] + t*(vertices[i][0] - pointLightPosition[0]);
            boundary_point[1] = pointLightPosition[1] + t*(vertices[i][1] - pointLightPosition[1]);
            boundary_point[2] = boundary_z;
            boundary_points.push_back(boundary_point);
        }
    }*/

    // run gradient descent on the boundary points to find their optimal positions such that they satisfy Fermat's principle
    /*if (!use_reflective_caustics && use_point_src) {
        for (int i=0; i<boundary_points.size(); i++) {
            for (int iteration=0; iteration<100000; iteration++) {
                double grad_x;
                double grad_y;
                gradient(pointLightPosition, boundary_points[i], vertices[i], 1.0, refractive_index, grad_x, grad_y);

                boundary_points[i][0] -= 0.1 * grad_x;
                boundary_points[i][1] -= 0.1 * grad_y;

                // if magintude of both is low enough
                if (grad_x*grad_x + grad_y*grad_y < 0.000001) {
                    break;
                }
            }
        }
    }*/

    for(int i = 0; i < vertices.size(); i++) {
        std::vector<double> incidentLight(3);
        std::vector<double> transmitted = {
            target_pts[i][0] - vertices[i][0],
            target_pts[i][1] - vertices[i][1],
            target_pts[i][2] - vertices[i][2]
        };

        if (use_point_src) {
            incidentLight[0] = vertices[i][0] - pointLightPosition[0];
            incidentLight[1] = vertices[i][1] - pointLightPosition[1];
            incidentLight[2] = vertices[i][2] - pointLightPosition[2];
        } else {
            incidentLight[0] = 0;
            incidentLight[1] = 0;
            incidentLight[2] = -1;
        }

        transmitted = normalize_vec(transmitted);
        incidentLight = normalize_vec(incidentLight);

        std::vector<double> normal(3);
        if (use_reflective_caustics) {
            normal[0] = ((transmitted[0]) + incidentLight[0]) * 1.0f;
            normal[1] = ((transmitted[1]) + incidentLight[1]) * 1.0f;
            normal[2] = ((transmitted[2]) + incidentLight[2]) * 1.0f;
        } else {
            normal[0] = ((transmitted[0]) - (incidentLight[0]) * refractive_index) * -1.0f;
            normal[1] = ((transmitted[1]) - (incidentLight[1]) * refractive_index) * -1.0f;
            normal[2] = ((transmitted[2]) - (incidentLight[2]) * refractive_index) * -1.0f;
        }

        normal = normalize_vec(normal);

        desiredNormals.push_back(normal);
    }

    return desiredNormals;
}

int main(int argc, char** argv)
{
  setlocale(LC_ALL,"C");

  InputParser input(argc, argv);
  
  MatrixXd density_src;
  MatrixXd density_trg;

  std::vector<Eigen::Vector2d> vertex_positions;
  normal_integration normal_int;

  if(input.cmdOptionExists("-help") || input.cmdOptionExists("-h")){
    output_usage();
    return 0;
  }

  CLIopts opts;
  if(!opts.load(input)){
    std::cerr << "invalid input" << std::endl;
    output_usage();
    return EXIT_FAILURE;
  }

  if(!load_input_density(opts.filename_src, density_src))
  {
    std::cout << "Failed to load input \"" << opts.filename_src << "\" -> abort.";
    exit(EXIT_FAILURE);
  }
    
  if(!load_input_density(opts.filename_trg, density_trg))
  {
    std::cout << "Failed to load input \"" << opts.filename_trg << "\" -> abort.";
    exit(EXIT_FAILURE);
  }

  TransportMap tmap_src = runOptimalTransport(density_src, opts);
  TransportMap tmap_trg = runOptimalTransport(density_trg, opts);

  //Mesh mesh(1.0, 1.0/2, opts.resolution, (int)(opts.resolution/2));
  Mesh mesh(1.0, 1.0, opts.resolution, opts.resolution);
  
  mesh.build_vertex_to_triangles();

  normal_int.initialize_data(mesh);
  
  //export_triangles_to_svg(mesh.source_points, mesh.triangles, 1, 1, opts.resolution, opts.resolution, "../triangles.svg", 0.5);
  //export_grid_to_svg(mesh.source_points, 1, 1, opts.resolution, opts.resolution, "../grid.svg", 0.5);

  scaleAndTranslatePoints(mesh.source_points, 1.0, 1.0, 1.0 / opts.resolution);
  
  for (int i=0; i<mesh.source_points.size(); i++)
  {
    Eigen::Vector2d point = {mesh.source_points[i][0], mesh.source_points[i][1]};
    vertex_positions.push_back(point);
  }

  applyTransportMapping(tmap_src, tmap_trg, density_trg, vertex_positions);
  
  std::vector<std::vector<double>> trg_pts;
  for (int i=0; i<mesh.source_points.size(); i++)
  {
    std::vector<double> point = {vertex_positions[i].x(), vertex_positions[i].y(), 0};
    trg_pts.push_back(point);
  }

  //export_grid_to_svg(trg_pts, 1, 0.5, opts.resolution, opts.resolution, "../grid.svg", 0.5);

  std::vector<std::vector<double>> desired_normals;

  //scalePoints(trg_pts, {8, 8, 0}, {0.5, 0.5, 0});
  rotatePoints(trg_pts, {0, 0, 0});
  translatePoints(trg_pts, {0, 0, -opts.focal_l});

  double r = 1.55;

  mesh.calculate_vertex_laplacians();

  for (int i=0; i<10; i++)
  {
      double max_z = -10000;

      for (int j = 0; j < mesh.source_points.size(); j++) {
        if (max_z < mesh.source_points[j][2]) {
          max_z = mesh.source_points[j][2];
        }
      }

      for (int j = 0; j < mesh.source_points.size(); j++) {
          mesh.source_points[j][2] -= max_z;
      }
      

      std::vector<std::vector<double>> normals = fresnelMapping(mesh.source_points, trg_pts, r);

      normal_int.perform_normal_integration(mesh, normals);

      //std::vector<std::vector<double>> vertex_normals = normal_int.calculate_vertex_normals(mesh);

      //std::vector<double> incidentLight(3);
      //incidentLight[0] = 0;
      //incidentLight[1] = 0;
      //incidentLight[2] = -1;

      //std::vector<int32_t> plane_triangle = mesh.triangles[0];

      //std::vector<double> plane_normal = calc_plane_normal(trg_pts[plane_triangle[0]], trg_pts[plane_triangle[0]], trg_pts[plane_triangle[0]]);

      //std::vector<std::vector<double>> intersections(vertex_normals.size());

      /*std::vector<double> pointLightPosition(3);
      pointLightPosition[0] = 0.5;
      pointLightPosition[1] = 0.5;
      pointLightPosition[2] = 0.5;

      for (int i = 0; i < vertex_normals.size(); i++)
      {
        vertex_normals[i][0] *= -1.0;
        vertex_normals[i][1] *= -1.0;
        vertex_normals[i][2] *= -1.0;

        std::vector<double> incidentLight(3);
        incidentLight[0] = mesh.source_points[i][0] - pointLightPosition[0];
        incidentLight[1] = mesh.source_points[i][1] - pointLightPosition[1];
        incidentLight[2] = mesh.source_points[i][2] - pointLightPosition[2];

        std::vector<double> intersectionPoint(3);
        std::vector<double> refracted = refract(vertex_normals[i], incidentLight, r, 1.0);

        refracted[0] /= -(refracted[2] + mesh.source_points[i][2]);
        refracted[1] /= -(refracted[2] + mesh.source_points[i][2]);
        refracted[2] /= -(refracted[2] + mesh.source_points[i][2]);

        refracted[0] += mesh.source_points[i][0];
        refracted[1] += mesh.source_points[i][1];
        refracted[2] += mesh.source_points[i][2];

        intersections[i] = refracted;
      }*/

      //export_triangles_to_svg(intersections, mesh.triangles, 1, 1, opts.resolution, opts.resolution, "../triangles.svg", 0.5);
      //export_grid_to_svg(intersections, 1, 1, opts.resolution, opts.resolution, "../intersections.svg", 0.5);

      //mesh.save_solid_obj_source(0.4, "../output.obj");
  }

  mesh.save_solid_obj_source(0.2, "../output.obj");
}
