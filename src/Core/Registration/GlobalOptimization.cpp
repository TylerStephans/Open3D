// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2017 Jaesik Park <syncle@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "GlobalOptimization.h"

#include <iostream> // for debugging
#include <fstream> // for debugging
#include <Eigen/Dense>
#include <json/json.h>
#include <Core/Utility/Console.h>
#include <Core/Utility/Eigen.h>

namespace three{

namespace {

const int MAX_ITER = 100;

bool stopping_criterion(/* what could be an input for this function? */) {
	return false;
}

/// todo: we may also do batch inverse for T_j
inline Eigen::Vector6d GetDiffVec(const Eigen::Matrix4d &X_inv, 
		const Eigen::Matrix4d &T_i, const Eigen::Matrix4d &T_j)
{
	Eigen::Matrix4d temp;
	temp.noalias() = X_inv * T_j.inverse() * T_i;
	return TransformMatrix4dToVector6d(temp);
}

}	// unnamed namespace

std::shared_ptr<PoseGraph> GlobalOptimization(const PoseGraph &pose_graph)
{
	int n_nodes = (int)pose_graph.nodes_.size();
	int n_edges = (int)pose_graph.edges_.size();

	PrintDebug("Optimizing PoseGraph having %d edges and %d nodes\n", 
			n_nodes, n_edges);

	Eigen::MatrixXd J(n_edges, n_nodes * 6);
	Eigen::VectorXd r(n_edges);
	std::vector<Eigen::Matrix4d> node_matrix_array;
	std::vector<Eigen::Matrix4d> xinv_matrix_array;
	node_matrix_array.resize(n_nodes);
	xinv_matrix_array.resize(n_edges);
	
	//x.setZero();
	for (int iter_node = 0; iter_node < n_nodes; iter_node++) {
		const PoseGraphNode &t = pose_graph.nodes_[iter_node];
		node_matrix_array[iter_node] = t.pose_;
	}
	for (int iter_edge = 0; iter_edge < n_edges; iter_edge++) {
		const PoseGraphEdge &t = pose_graph.edges_[iter_edge];
		xinv_matrix_array[iter_edge] = t.transformation_.inverse();
	}

	Eigen::VectorXd line_process(n_edges - (n_nodes - 1));
	line_process.setOnes();

	// main iteration
	for (int iter = 0; iter < MAX_ITER; iter++) {

		J.setZero();
		r.setZero();		
		double total_residual2 = 0.0;
	
		// depends on the definition of nodes maybe need to do (n_nodes + 1)?
		int line_process_cnt = 0;

		// building jacobian for loop edges
		// todo: this initialization should be done one time.
		for (int iter_edge = 0; iter_edge < n_edges; iter_edge++) {
			const PoseGraphEdge &t = pose_graph.edges_[iter_edge];
			Eigen::Vector6d trans_vec = GetDiffVec(
					xinv_matrix_array[iter_edge],
					node_matrix_array[t.source_node_id_],
					node_matrix_array[t.target_node_id_]);
			double residual = sqrt(trans_vec.transpose() * t.information_ * trans_vec);
			Eigen::Vector6d J_vec = 
					(trans_vec.transpose() * t.information_) / residual;
			double line_process_sqrt = 1.0;
			//if (abs(target_node_id - source_node_id) != 1) // loop edge
			//	line_process_sqrt = sqrt(line_process(line_process_cnt++));
			//std::cout << iter_edge << " line_process_sqrt " << line_process_sqrt << std::endl;
			int row_id = iter_edge;
			J.block<1, 6>(row_id, t.source_node_id_ * 6) = 
					line_process_sqrt * J_vec;
			J.block<1, 6>(row_id, t.target_node_id_ * 6) = 
					line_process_sqrt * -J_vec; 
			r(row_id) = residual;
			total_residual2 += line_process_sqrt * line_process_sqrt * residual * residual;
		}
		// solve equation
		Eigen::MatrixXd JtJ = J.transpose() * J;
		Eigen::MatrixXd Jtr = J.transpose() * r;
		
		//std::ofstream outfile;
		//outfile.open("JtJ.txt");
		//outfile << JtJ;
		//outfile.close();
		//outfile.open("Jtr.txt");
		//outfile << Jtr;
		//outfile.close();
		
		//bool is_success;
		//Eigen::VectorXd x_delta;
		//std::tie(is_success, x_delta) = SolveLinearSystem(JtJ, Jtr); // determinant is always inf.
		Eigen::VectorXd delta = -JtJ.ldlt().solve(Jtr);
		
		// change 6d vector to matrix form
		// to save time for computing matrix form multiple times.
		for (int iter_node = 0; iter_node < n_nodes; iter_node++) {
			Eigen::Vector6d delta_iter = delta.block<6, 1>(iter_node * 6, 0);
			//std::cout << x_iter_node << std::endl;
			node_matrix_array[iter_node] =
					TransformVector6dToMatrix4d(delta_iter) *
					node_matrix_array[iter_node];
		}
		
		// sub-iteration #2
		// update line process
		// just right after sub-iteration #1?
		line_process_cnt = 0;
		for (int iter_edge = 0; iter_edge < n_edges; iter_edge++) {
			const PoseGraphEdge &t = pose_graph.edges_[iter_edge];
			if (abs(t.target_node_id_ - t.source_node_id_) != 1) { // only for loop edge
				Eigen::Vector6d diff_vec = GetDiffVec(
						xinv_matrix_array[iter_edge],
						node_matrix_array[t.source_node_id_],
						node_matrix_array[t.target_node_id_]);
				double residual_square = 
						diff_vec.transpose() * t.information_ * diff_vec;
				double temp = 1.0 / (1.0 + residual_square);
				line_process(line_process_cnt++) = temp * temp;
			}
		}
		// adding stopping criterion
		PrintDebug("Iter : %d, residual : %e\n", iter, total_residual2);

		//std::cout << line_process << std::endl;

		if (stopping_criterion())
			break;
	}

	std::shared_ptr<PoseGraph> pose_graph_refined = std::make_shared<PoseGraph>();
	for (int iter_node = 0; iter_node < n_nodes; iter_node++) {
		PoseGraphNode new_node(node_matrix_array[iter_node]);
		pose_graph_refined->nodes_.push_back(new_node);
	}
	for (int iter_edge = 0; iter_edge < n_edges; iter_edge++) {
		const PoseGraphEdge &t = pose_graph.edges_[iter_edge];
		Eigen::Vector6d diff_vec = GetDiffVec(
				xinv_matrix_array[iter_edge],
				node_matrix_array[t.source_node_id_],
				node_matrix_array[t.target_node_id_]);
		PoseGraphEdge new_edge(t.source_node_id_, t.target_node_id_, 
				TransformVector6dToMatrix4d(diff_vec),
				Eigen::Matrix6d::Identity(), false);
		pose_graph_refined->edges_.push_back(new_edge);
	}

	return pose_graph_refined;
}

}	// namespace three
