#include "cinematic_match.hpp"
#include <stdexcept>
#include <iostream>
using namespace qs;
static void check(bool yes,const char *s){if(!yes)throw std::runtime_error(s);}
int main(){
    try{
        using namespace qs::cinematic_detail;
        SceneStamp first{1,2,88,engine_sequence},other{1,3,17,engine_sequence};
        Match match({first,other},100);
        check(!match.plan({1,8,3,engine_sequence},500,200).has_value(),"wrong scene was accepted");
        auto target=match.plan({1,2,9,engine_sequence},500,210);
        check(target && *target==88,"forward seek not issued");
        check(!match.plan({1,2,9,engine_sequence},500,211).has_value(),"duplicate seek issued");
        match.issued(first,false);
        check(!match.result(230).matched,"unobserved frame was confirmed");
        auto backwards=match.plan({1,3,86,engine_sequence},100,235);
        check(backwards && *backwards==17,"backward jump not supported");
        match.issued(other,true);
        check(match.result(235).matched==0,"the call alone was counted as continuation");
        match.plan({1,3,18,engine_sequence},100,245);
        check(match.result(245).matched==1 && !match.result(245).finished,"next live update missing");
        check(match.result(20000).finished && match.result(20000).matched==1,"timeout must be partial");
        Match exact({first},0);
        check(!exact.plan(first,200,10).has_value() && exact.result(10).matched==1 && exact.result(10).finished,"exact existing cursor");
        Match outside({first},0);
        check(!outside.plan({1,2,1,engine_sequence},60,10).has_value() && !outside.result(8000).matched,"out of range seek");
        Match duplicate({first,first},0);
        check(!duplicate.plan(first,100,1).has_value()&&!duplicate.result(10000).matched,"ambiguous targets");
        SceneStamp film{10,20,100,bink_movie};
        Match movie({film},0);
        check(!movie.plan({10,20,5,engine_sequence},120,10).has_value(),"wrong media kind accepted");
        check(!movie.plan({10,19,5,bink_movie},120,12).has_value(),"different movie accepted");
        check(movie.plan({10,20,5,bink_movie},120,19)==100,"identified movie failed to seek");
        movie.issued(film,true);
        movie.plan({10,20,46,bink_movie},120,24);
        check(!movie.result(24).matched,"an earlier codec keyframe counted as a match");
        movie.plan({10,20,100,bink_movie},120,90);
        check(movie.result(90).matched==1,"verified decoder frame ignored");
        film.frame=1;Match opening({film},0);
        check(!opening.plan({10,20,25,bink_movie},120,200).has_value(),"Bink cannot Goto first frame via this wrapper");
        opening.plan(film,120,350);check(opening.result(350).matched==1,"natural first frame ignored");
        std::cout<<"Verified scene identity, frame bounds, forward/backward seek, confirmation, timeout, and duplicate suppression.\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
